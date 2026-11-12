"""Reject a full FFmpeg SDK or a cached build with extra codecs/protocols."""
import argparse
from pathlib import Path
import re

def verify(prefix):
    config=(prefix/'licenses/ffmpeg-config.mak').read_text(encoding='utf-8')
    active=set(re.findall(r'^(CONFIG_[A-Z0-9_]+)=yes$',config,re.M))
    for suffix,expected in [('DECODER',{'CONFIG_PRORES_DECODER'}),('DEMUXER',{'CONFIG_MOV_DEMUXER'}),('PROTOCOL',{'CONFIG_FILE_PROTOCOL'})]:
        actual={name for name in active if name.endswith('_'+suffix)}
        if actual!=expected:raise ValueError(f'Not the minimal build: {suffix}={sorted(actual)}')
    if 'CONFIG_NETWORK' in active:raise ValueError('Network support must be disabled')
    for name in ['avcodec-62.dll','avformat-62.dll','avutil-60.dll','swscale-9.dll','swresample-6.dll']:
        f=prefix/'bin'/name
        if not f.is_file() or f.stat().st_size>4*1024*1024:raise ValueError(f'Missing or oversized minimal library: {f}')
    print('Minimal Windows FFmpeg verified: ProRes + MOV + file only')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--prefix',type=Path,required=True)
    verify(parser.parse_args().prefix)
