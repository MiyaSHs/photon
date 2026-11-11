"""Usage: python3 -I scripts/check_exports.py <dll> <ExpectedExport>...  - exit 1 unless the named exports are exactly as given."""
import sys, pefile
pe = pefile.PE(sys.argv[1])
names = sorted(e.name.decode() for e in pe.DIRECTORY_ENTRY_EXPORT.symbols if e.name) if hasattr(pe, 'DIRECTORY_ENTRY_EXPORT') else []
want = sorted(sys.argv[2:])
print(sys.argv[1], 'exports:', names)
if 'BuiltDebug' in names: print('FAIL: exports BuiltDebug (Sys_LoadModule would reject it)'); sys.exit(1)
if names != want: print('FAIL: expected', want); sys.exit(1)
print('exports OK')
