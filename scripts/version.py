from pathlib import Path
import re
s=(Path(__file__).resolve().parents[1]/'src/AlphaFixVersion.h').read_text(encoding='utf-8')
print('.'.join(re.search(r'#define\s+ALPHAFIX_'+part+r'_VERSION\s+(\d+)',s).group(1) for part in ['MAJOR','MINOR','BUG']))
