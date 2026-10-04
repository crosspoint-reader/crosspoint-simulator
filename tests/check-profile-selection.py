"""Compile the public board contract and reject incompatible selections."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
profiles = [
    ([], "xteink_x4", 0, 0, 0),
    (["SIMULATOR_DEVICE_X3"], "xteink_x3", 0, 0, 0),
    (["SIMULATOR_DEVICE_X4_PRO"], "xteink_x4_pro", 1, 1, 1),
    (["SIMULATOR_DEVICE_X4_CLASSIC"], "xteink_x4_classic", 0, 0, 0),
    (["SIMULATOR_DEVICE_STICKY"], "sticky", 1, 0, 0),
    (["SIMULATOR_DEVICE_PAPERMONO"], "m5stack_paper_mono", 1, 0, 1),
    (["SIMULATOR_DEVICE_METALIO_EINK4"], "metalio_eink4", 1, 1, 0),
]
with tempfile.TemporaryDirectory(prefix="simulator-profiles-") as directory:
    folder = Path(directory)
    source = folder / "contract.cpp"
    source.write_text('''#include <BoardConfig.h>
#include <cassert>
#include <cstdlib>
#include <cstring>
int main(int, char **argv) {
  assert(std::strcmp(BoardConfig::ACTIVE.name, argv[1]) == 0);
  assert(BoardConfig::hasTouch() == bool(std::atoi(argv[2])));
  assert(BoardConfig::hasHomeKey() == bool(std::atoi(argv[3])));
  assert(BoardConfig::hasPwmFrontlight() == bool(std::atoi(argv[4])));
  assert(FREEINK_CAP_TOUCH == int(BoardConfig::hasTouch()));
  assert(FREEINK_CAP_FRONTLIGHT == int(BoardConfig::hasPwmFrontlight()));
  assert(FREEINK_DEVICE_X4 + FREEINK_DEVICE_X3 + FREEINK_DEVICE_X4PRO +
         FREEINK_DEVICE_X4CLASSIC + FREEINK_DEVICE_STICKY +
         FREEINK_DEVICE_PAPERMONO + FREEINK_DEVICE_METALIO_EINK4 == 1);
}
''')
    command = ["c++", "-std=c++20", f"-I{root / 'src'}", "-DFREEINK_DEVICE_X4=1", str(source)]
    for flags, name, touch, home, light in profiles:
        binary = folder / name
        subprocess.run(command + [f"-D{flag}" for flag in flags] + ["-o", str(binary)], check=True)
        subprocess.run([str(binary), name, str(touch), str(home), str(light)], check=True)
    negatives = [
        (["SIMULATOR_DEVICE_METALIO_EINK4", "SIMULATOR_DISPLAY_UC8179"], "uses SSD1677"),
        (["SIMULATOR_DEVICE_METALIO_EINK4", "SIMULATOR_DISPLAY_UC8279"], "uses SSD1677"),
    ]
    negatives += [(["SIMULATOR_DEVICE_METALIO_EINK4"] + flags, "at most one simulated device")
                  for flags, *_ in profiles if flags and "SIMULATOR_DEVICE_METALIO_EINK4" not in flags]
    for flags, diagnostic in negatives:
        result = subprocess.run(command + [f"-D{flag}" for flag in flags] + ["-fsyntax-only"], capture_output=True, text=True)
        assert result.returncode != 0 and diagnostic in result.stderr, (flags, result.stderr)
print("Seven device contracts and seven invalid Metalio selections passed")
