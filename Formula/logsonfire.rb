class Logsonfire < Formula
  desc "Local log analysis and compact evidence for agents"
  homepage "https://github.com/danoli3/LogsOnFire"
  url "https://github.com/danoli3/LogsOnFire/releases/download/v0.2.0-rc.1/logsonfire-0.2.0-rc.1-source.tar.gz"
  version "0.2.0-rc.1"
  sha256 "01e6a52cd196db2d604c7a57e755c7927ac7e124d9dd5b0e52512ee9d6771c3e"
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
