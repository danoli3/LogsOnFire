class Logsonfire < Formula
  desc "Local log analysis and compact evidence for agents"
  homepage "https://github.com/danoli3/LogsOnFire"
  url "https://github.com/danoli3/LogsOnFire/releases/download/v0.2.0-rc.2/logsonfire-0.2.0-rc.2-source.tar.gz"
  version "0.2.0-rc.2"
  sha256 "4675c6170dfc0c5432c94bf927b9e3ff39b8eab50d103f1b7a771be0ef27d048"
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
