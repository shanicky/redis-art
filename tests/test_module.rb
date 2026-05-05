# frozen_string_literal: true

require "socket"
require "timeout"
require "tmpdir"
require "fileutils"

ROOT = File.expand_path("..", __dir__)
MODULE_PATH = File.join(ROOT, "rtree.so")

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

abort "rtree.so not found; run make first" unless File.exist?(MODULE_PATH)
abort "redis-server not found" unless system("command -v redis-server >/dev/null")
abort "redis-cli not found" unless system("command -v redis-cli >/dev/null")

port = free_port
dir = Dir.mktmpdir("rtree-test")
pid = spawn("redis-server",
            "--port", port.to_s,
            "--save", "",
            "--appendonly", "no",
            "--dir", dir,
            "--loadmodule", MODULE_PATH,
            out: File::NULL,
            err: File::NULL)

begin
  Timeout.timeout(5) do
    loop do
      break if system("redis-cli -p #{port} ping >/dev/null 2>&1")

      sleep 0.05
    end
  end

  raise "SET new" unless redis_cli(port, "RTREE.SET", "h", "b", "2") == ["1"]
  raise "SET new a" unless redis_cli(port, "RTREE.SET", "h", "a", "1") == ["1"]
  raise "SET update" unless redis_cli(port, "RTREE.SET", "h", "b", "22") == ["0"]
  raise "GET" unless redis_cli(port, "RTREE.GET", "h", "b") == ["22"]
  raise "LEN" unless redis_cli(port, "RTREE.LEN", "h") == ["2"]
  raise "KEYS" unless redis_cli(port, "RTREE.KEYS", "h") == %w[a b]
  raise "GETALL" unless redis_cli(port, "RTREE.GETALL", "h") == %w[a 1 b 22]

  redis_cli(port, "RTREE.SET", "h", "aa", "11")
  redis_cli(port, "RTREE.SET", "h", "ab", "12")
  raise "PREFIX" unless redis_cli(port, "RTREE.GETPREFIX", "h", "a") == %w[a 1 aa 11 ab 12]
  raise "RANGE" unless redis_cli(port, "RTREE.RANGE", "h", "aa", "b") == %w[aa 11 ab 12 b 22]
  raise "REVRANGE" unless redis_cli(port, "RTREE.REVRANGE", "h", "aa", "b") == %w[b 22 ab 12 aa 11]
  raise "SCAN no match" unless redis_cli(port, "RTREE.SCAN", "h", "0", "COUNT", "2") == %w[2 a 1 aa 11]
  raise "SCAN match page 1" unless redis_cli(port, "RTREE.SCAN", "h", "0", "MATCH", "a*", "COUNT", "2") == %w[2 a 1 aa 11]
  raise "SCAN match page 2" unless redis_cli(port, "RTREE.SCAN", "h", "2", "MATCH", "a*", "COUNT", "2") == %w[0 ab 12]
  raise "SCAN" unless redis_cli(port, "RTREE.SCAN", "h", "0", "MATCH", "a*", "COUNT", "10") == %w[0 a 1 aa 11 ab 12]
  raise "DEL" unless redis_cli(port, "RTREE.DEL", "h", "aa", "missing") == ["1"]
  raise "EXISTS" unless redis_cli(port, "RTREE.EXISTS", "h", "aa") == ["0"]

  puts "test_module: ok"
ensure
  system("redis-cli -p #{port} shutdown nosave >/dev/null 2>&1")
  Process.wait(pid)
  FileUtils.remove_entry(dir)
end
