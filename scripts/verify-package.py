"""Check product filenames, flat layout and minimal dependency sizes."""
import sys,zipfile,plistlib
from pathlib import Path
for filename in sys.argv[1:]:
    with zipfile.ZipFile(filename) as z:
        assert z.testzip() is None,'Corrupt archive'
        names=[n for n in z.namelist() if not n.startswith('__MACOSX/')]
        assert all(n.startswith('Alpha Fix/') for n in names), 'Expected Alpha Fix/ as the only top-level folder'
        assert not any('Support Files/' in n or '/Plug-ins/' in n for n in names),'Nested installation directories are not permitted'
        if any(n.endswith('.aex') for n in names):
            assert [n for n in names if n.endswith('.aex')]==['Alpha Fix/Alpha_Fix.aex']
            codec=z.getinfo('Alpha Fix/alphafix.ffmpeg/avcodec-62.dll')
            assert codec.file_size<4*1024*1024,'Full FFmpeg avcodec DLL was packaged'
            config=z.read('Alpha Fix/licenses/ffmpeg-config.mak').decode()
            assert '\nCONFIG_PRORES_DECODER=yes' in config
            assert '\nCONFIG_MOV_DEMUXER=yes' in config
        else:
            info=plistlib.loads(z.read('Alpha Fix/Alpha_Fix.plugin/Contents/Info.plist'))
            assert info['CFBundleExecutable']=='Alpha_Fix'
            assert info['CFBundleName']=='Alpha Fix'
            assert 'Alpha Fix/Alpha_Fix.plugin/Contents/MacOS/Alpha_Fix' in names
    print(Path(filename).name+': flat package, binary names and dependencies verified')
