#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/../.."
work=$(mktemp -d /private/tmp/botw-game-import.XXXXXX)
trap 'rm -rf "$work"' EXIT
python3 tests/game_import/make_fixture.py "$work/fixtures"
swiftc -module-cache-path "$work/cache" -warnings-as-errors ios/src/BotWL10n.swift ios/src/BotWZipImport.swift ios/src/BotWGameImport.swift tests/game_import/main.swift -lz -o "$work/test"
"$work/test" "$work/fixtures" "$work/runs"
python3 - "$work/console_locale.inc" <<'PY'
from pathlib import Path
import sys
source = Path('runtime/src/coreinit/misc.cpp').read_text()
start = source.index('struct ConsoleLocale')
end = source.index('}  // namespace', start)
Path(sys.argv[1]).write_text(source[start:end])
PY
clang++ -std=c++17 -Wall -Wextra -Werror -I "$work" tests/game_import/runtime_locale.cpp -o "$work/locale-test"
"$work/locale-test"
