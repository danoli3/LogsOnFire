class Logsonfire < Formula
  desc "Local log analysis and compact evidence for agents"
  homepage "https://github.com/danoli3/LogsOnFire"
  url "https://github.com/danoli3/LogsOnFire/releases/download/v0.2.0-rc.3/logsonfire-0.2.0-rc.3-source.tar.gz"
  version "0.2.0-rc.3"
  sha256 "eb4b3b4dcff76fe7d2f0292afb6d3dbbe229ce107925ce68ea0e9fdd30e5a7de"
  license "MIT"

  depends_on "cmake" => :build
  depends_on "python"
  depends_on "sqlite"

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args, "-DBUILD_TESTING=OFF"
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  test do
    (testpath/"ok.log").write("** BUILD SUCCEEDED **\n")
    assert_match '"status":"ok"', shell_output("#{bin}/logsonfire --output json --db #{testpath}/logs.db ingest #{testpath}/ok.log")
  end
end
