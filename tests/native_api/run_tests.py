"""Compile the actual native API C body with host hardware/RTOS adapters.

Requires Python 3 and GCC on Linux. No Home Assistant modules are used.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/driver/drv_openbeken_api.c"


def main():
    source = SOURCE.read_text()
    # Keep the production implementation and conditional compilation intact.
    # Replace firmware-only includes with host adapters, not protocol functions.
    source = source[source.index("#if ENABLE_DRIVER_OPENBEKEN_API"):]
    channel_types = sorted(set(re.findall(r"\bChType_\w+", source)))
    with tempfile.TemporaryDirectory(prefix="obka-tests-") as directory:
        directory = Path(directory)
        # Test the actual HTTP writer: socket zero is valid on lwIP. The fake
        # request uses an explicit invalid descriptor instead of a real socket.
        http = (ROOT / "src/httpserver/new_http.c").read_text()
        http = http[http.index("int postany("):http.index("int poststr(")]
        (directory / "http_reply_body.inc").write_text(http)
        http_executable = directory / "http_reply_tests"
        subprocess.run([
            os.environ.get("CC", "gcc"), "-std=gnu99", "-g", "-O1",
            "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-no-pie", "-I", str(directory),
            "-I", str(ROOT / "src/httpserver"),
            str(ROOT / "tests/native_api/test_http_reply.c"),
            "-o", str(http_executable)
        ], check=True)
        subprocess.run([str(http_executable)], check=True, timeout=10)
        # The hook must update SDK headers reproducibly and accept a second run.
        header = directory / "sdk/include/lwip/apps/mdns_opts.h"
        header.parent.mkdir(parents=True)
        header.write_text("#define MDNS_MAX_SERVICES 1\n")
        hook = ROOT / "platforms/BK723x/native_api_mdns.sh"
        for _ in range(2):
            subprocess.run(["sh", str(hook), str(directory / "sdk")], check=True)
            assert header.read_text().strip() == "#define MDNS_MAX_SERVICES               2"
        header.write_text("#define MDNS_MAX_SERVICES 3\n")
        result = subprocess.run(["sh", str(hook), str(directory / "sdk")], capture_output=True)
        assert result.returncode != 0
        print("PASS SDK hook: two services, idempotence, incompatible configuration rejected", flush=True)
        (directory / "native_api_body.inc").write_text(source)
        (directory / "channel_types.h").write_text(
            "enum {" + ",".join(channel_types) + "};\n"
        )
        executable = directory / "native_api_tests"
        subprocess.run([
            os.environ.get("CC", "gcc"), "-std=gnu99", "-g", "-O1",
            "-Wall", "-Wextra", "-Werror=implicit-function-declaration",
            "-Wno-unused-function", "-Wno-unused-variable", "-Wno-sign-compare",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-I", str(directory), "-I", str(ROOT / "src/cJSON"),
            str(ROOT / "tests/native_api/test_server.c"),
            str(ROOT / "src/cJSON/cJSON.c"), "-pthread", "-lm", "-o", str(executable)
        ], check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
        mdns = (ROOT / "src/driver/drv_mdns.c").read_text()
        mdns = mdns[mdns.index("static void DRV_MDNS_UpdateServices"):]
        mdns = mdns[:mdns.index("static void DRV_MDNS_StartOrRestart")]
        (directory / "mdns_services.inc").write_text(mdns)
        for enabled, slots in ((1, 1), (1, 2), (0, 1), (0, 2)):
            subprocess.run([
                os.environ.get("CC", "gcc"), "-std=gnu99", "-g", "-O1",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                f"-DMDNS_MAX_SERVICES={slots}", f"-DENABLE_DRIVER_OPENBEKEN_API={enabled}",
                "-Werror=implicit-function-declaration", "-I", str(directory),
                str(ROOT / "tests/native_api/test_mdns.c"), "-o", str(executable)
            ], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)


if __name__ == "__main__":
    main()
