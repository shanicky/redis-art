# frozen_string_literal: true

require "socket"
require "timeout"
require "tmpdir"
require "fileutils"
require "find"

ROOT = File.expand_path("..", __dir__)
MODULE_PATH = File.join(ROOT, "rtree.so")
# redis-cli --raw renders an empty array as one blank line.
EMPTY_ARRAY = [""].freeze

def free_port
  server = TCPServer.new("127.0.0.1", 0)
  port = server.addr[1]
  server.close
  port
end

def redis_cli(port, *args)
  output = IO.popen(["redis-cli", "-p", port.to_s, "--raw", *args], err: %i[child out], &:read)
  raise "redis-cli failed: #{output}" unless $?.success?

  output.lines.map(&:chomp)
end

def redis_map(port, *args)
  lines = redis_cli(port, *args)
  raise "expected an even number of map fields: #{lines.inspect}" unless lines.length.even?

  Hash[*lines]
end

def read_pubsub_line(io)
  Timeout.timeout(5) do
    line = io.gets
    raise "subscription closed" if line.nil?

    line.chomp
  end
end

def read_pubsub_message(io)
  loop do
    kind = read_pubsub_line(io)
    next unless kind == "message"

    return [read_pubsub_line(io), read_pubsub_line(io)]
  end
end

def with_subscription(port, channel)
  io = IO.popen(["redis-cli", "-p", port.to_s, "--raw", "SUBSCRIBE", channel], "r")
  read_pubsub_line(io)
  read_pubsub_line(io)
  read_pubsub_line(io)
  yield io
ensure
  if io
    begin
      Process.kill("TERM", io.pid)
    rescue Errno::ESRCH
      nil
    end
    io.close unless io.closed?
  end
end

def wait_for_redis(port)
  Timeout.timeout(5) do
    loop do
      break if system("redis-cli -p #{port} ping >/dev/null 2>&1")

      sleep 0.05
    end
  end
end

def start_redis(port, dir)
  pid = spawn("redis-server",
              "--port", port.to_s,
              "--save", "",
              "--appendonly", "no",
              "--dir", dir,
              "--loadmodule", MODULE_PATH,
              out: File::NULL,
              err: File::NULL)
  wait_for_redis(port)
  pid
end

def stop_redis(port, pid, save: false)
  mode = save ? "save" : "nosave"
  system("redis-cli -p #{port} shutdown #{mode} >/dev/null 2>&1")
  Process.wait(pid)
end

def wait_for_aof_rewrite(port)
  Timeout.timeout(5) do
    loop do
      info = redis_cli(port, "INFO", "persistence")
      line = info.find { |item| item.start_with?("aof_rewrite_in_progress:") }
      break if line&.end_with?(":0")

      sleep 0.05
    end
  end
end

def appendonly_file_created?(dir)
  Find.find(dir).any? { |path| File.file?(path) && File.basename(path).include?("appendonly") }
end

abort "rtree.so not found; run make first" unless File.exist?(MODULE_PATH)
abort "redis-server not found" unless system("command -v redis-server >/dev/null")
abort "redis-cli not found" unless system("command -v redis-cli >/dev/null")

port = free_port
dir = Dir.mktmpdir("rtree-test")
pid = nil

begin
  pid = start_redis(port, dir)

  raise "SET new" unless redis_cli(port, "RTREE.SET", "h", "b", "2") == ["1"]
  raise "SET new a" unless redis_cli(port, "RTREE.SET", "h", "a", "1") == ["1"]
  raise "SET update" unless redis_cli(port, "RTREE.SET", "h", "b", "22") == ["0"]
  raise "GET" unless redis_cli(port, "RTREE.GET", "h", "b") == ["22"]
  raise "LEN" unless redis_cli(port, "RTREE.LEN", "h") == ["2"]
  info = redis_map(port, "RTREE.INFO", "h")
  raise "INFO type" unless info["type"] == "rtree-art"
  raise "INFO encoding" unless info["encoding"] == "art"
  raise "INFO length" unless info["length"] == "2"
  raise "INFO memory_usage" unless info["memory_usage"].to_i.positive?
  missing_info = redis_map(port, "RTREE.INFO", "missing")
  raise "INFO missing length" unless missing_info["length"] == "0"
  raise "INFO missing memory_usage" unless missing_info["memory_usage"] == "0"
  raise "COMMAND DOCS" unless redis_cli(port, "COMMAND", "DOCS", "RTREE.INFO").include?("Return metadata about an rtree key.")
  raise "ACL read category" unless redis_cli(port, "ACL", "CAT", "read").include?("rtree.info")
  raise "ACL write category" unless redis_cli(port, "ACL", "CAT", "write").include?("rtree.set")
  raise "KEYS" unless redis_cli(port, "RTREE.KEYS", "h") == %w[a b]
  raise "GETALL" unless redis_cli(port, "RTREE.GETALL", "h") == %w[a 1 b 22]

  redis_cli(port, "RTREE.SET", "h", "aa", "11")
  redis_cli(port, "RTREE.SET", "h", "ab", "12")
  redis_cli(port, "RTREE.SET", "h", "", "empty")
  raise "VALS" unless redis_cli(port, "RTREE.VALS", "h") == %w[empty 1 11 12 22]
  raise "GETALL empty field" unless redis_cli(port, "RTREE.GETALL", "h") == ["", "empty", "a", "1", "aa", "11", "ab", "12", "b", "22"]
  raise "PREFIX" unless redis_cli(port, "RTREE.GETPREFIX", "h", "a") == %w[a 1 aa 11 ab 12]
  raise "RANGE" unless redis_cli(port, "RTREE.RANGE", "h", "aa", "b") == %w[aa 11 ab 12 b 22]
  raise "RANGE limit" unless redis_cli(port, "RTREE.RANGE", "h", "a", "z", "LIMIT", "2") == %w[a 1 aa 11]
  raise "RANGE limit zero" unless redis_cli(port, "RTREE.RANGE", "h", "a", "z", "LIMIT", "0") == EMPTY_ARRAY
  raise "REVRANGE" unless redis_cli(port, "RTREE.REVRANGE", "h", "aa", "b") == %w[b 22 ab 12 aa 11]
  raise "REVRANGE limit" unless redis_cli(port, "RTREE.REVRANGE", "h", "a", "z", "LIMIT", "2") == %w[b 22 ab 12]
  raise "SCAN no match" unless redis_cli(port, "RTREE.SCAN", "h", "0", "COUNT", "2") == ["2", "", "empty", "a", "1"]
  raise "SCAN match page 1" unless redis_cli(port, "RTREE.SCAN", "h", "0", "MATCH", "a*", "COUNT", "2") == %w[3 a 1 aa 11]
  raise "SCAN match page 2" unless redis_cli(port, "RTREE.SCAN", "h", "3", "MATCH", "a*", "COUNT", "2") == %w[0 ab 12]
  raise "SCAN" unless redis_cli(port, "RTREE.SCAN", "h", "0", "MATCH", "a*", "COUNT", "10") == %w[0 a 1 aa 11 ab 12]
  raise "LEN missing" unless redis_cli(port, "RTREE.LEN", "missing") == ["0"]
  raise "KEYS missing" unless redis_cli(port, "RTREE.KEYS", "missing") == EMPTY_ARRAY
  raise "DEL" unless redis_cli(port, "RTREE.DEL", "h", "aa", "missing") == ["1"]
  raise "EXISTS" unless redis_cli(port, "RTREE.EXISTS", "h", "aa") == ["0"]

  raise "notify config" unless redis_cli(port, "CONFIG", "SET", "notify-keyspace-events", "Kd") == ["OK"]
  with_subscription(port, "__keyspace@0__:events") do |sub|
    raise "event SET" unless redis_cli(port, "RTREE.SET", "events", "x", "1") == ["1"]
    raise "event set notification" unless read_pubsub_message(sub) == ["__keyspace@0__:events", "rtree.set"]
    raise "event DEL" unless redis_cli(port, "RTREE.DEL", "events", "x") == ["1"]
    raise "event del notification" unless read_pubsub_message(sub) == ["__keyspace@0__:events", "rtree.del"]
    raise "event empty notification" unless read_pubsub_message(sub) == ["__keyspace@0__:events", "rtree.empty"]
  end

  raise "SAVE" unless redis_cli(port, "SAVE") == ["OK"]
  stop_redis(port, pid, save: true)
  pid = start_redis(port, dir)
  raise "RDB reload" unless redis_cli(port, "RTREE.GETALL", "h") == ["", "empty", "a", "1", "ab", "12", "b", "22"]

  raise "BGREWRITEAOF" unless redis_cli(port, "BGREWRITEAOF") == ["Background append only file rewriting started"]
  wait_for_aof_rewrite(port)
  raise "AOF rewrite" unless appendonly_file_created?(dir)

  puts "test_module: ok"
ensure
  stop_redis(port, pid) if pid
  FileUtils.remove_entry(dir)
end
