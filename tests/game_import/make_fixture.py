#!/usr/bin/env python3
"""Synthetic archives only. sparse.zip has >4 GiB offsets but uses almost no physical storage."""
from pathlib import Path
import hashlib
import json
import struct as S
import subprocess
import sys
import warnings
import zipfile as Z

out = Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
rpx = b'Synthetic v208 executable for importer tests only\n'
(out / 'rpx.sha256').write_text(hashlib.sha256(rpx).hexdigest())
def tree(region='JP', version=208, title=None, lang=None):
    ids = {'JP':'00050000101C9300','US':'00050000101C9400','EU':'00050000101C9500'}
    packs = {'JP':['JPja'],'US':['USen','USfr','USes'],'EU':['EUen','EUde','EUfr','EUes','EUit','EUnl','EUru']}
    files = {'code/U-King.rpx':rpx, 'code/app.xml':b'<app/>', 'code/cos.xml':b'<app/>',
             'meta/meta.xml':f'<menu><title_id type="hexBinary" length="8">{title or ids[region]}</title_id><title_version type="unsignedInt" length="4">{version}</title_version></menu>'.encode(),
             'content/test.bin': bytes(range(256)) * 256}
    files.update({f'content/Pack/Bootup_{p}.pack':b'pack' for p in (lang or packs[region])})
    return files

def archive(name, files=None, prefix='', method=Z.ZIP_STORED, extras=None):
    with warnings.catch_warnings():
        warnings.simplefilter('ignore')
        with Z.ZipFile(out / (name+'.zip'), 'w', compression=method) as z:
            for path,data in (files or tree()).items(): z.writestr(prefix+path,data)
            for path,data in (extras or []): z.writestr(path,data)

archive('stored'); archive('deflate',method=Z.ZIP_DEFLATED)
archive('prefix',prefix='game/'); archive('unicode',prefix='Zelda – Wildnis/Spiel ü/')
archive('junk',extras=[('__MACOSX/._foo',b'x'),('.DS_Store',b'x'),('._foo',b'x'),('.hidden/code/U-King.rpx',b'ignored')])
for region in ('JP','US','EU'): archive(region,tree(region))
for name,files in [('old-version',tree(version=207)),('unknown-title',tree(title='1234')),('wrong-language',tree(lang=['USen']))]: archive(name,files)
f=tree(); del f['meta/meta.xml']; archive('missing-meta',f)
f=tree(); f['code/U-King.rpx']=b'wrong';archive('wrong-rpx',f)
archive('dlc',extras=[('aoc/content/dlc.bin',b'dlc'),('0005000C101C9300/meta/meta.xml',b'<menu/>')])
for name,path in [('dotdot','../evil'),('absolute','/evil'),('backslash','content\\evil'),('colon','content/a:b'),('empty-segment','content//bad'),('dot-segment','content/./bad'),('duplicate','code/app.xml')]: archive(name,extras=[(path,b'bad')])
archive('case',extras=[('code/APP.xml',b'ignored')])
info=Z.ZipInfo('content/link'); info.create_system=3; info.external_attr=0o120777<<16
archive('symlink',extras=[(info,b'/private/tmp')])
archive('bzip2',method=Z.ZIP_BZIP2)
raw=(out/'stored.zip').read_bytes(); cd=raw.index(b'PK\x01\x02')
for name,off,fmt,val in [('encrypted',cd+8,'H',1),('split',len(raw)-22+4,'H',1),('bad-central',cd,'I',0),('local-name',30,'B',ord('x')),('single-limit',cd+24,'I',0xffffffff)]:
    data=bytearray(raw);S.pack_into('<'+fmt,data,off,val);(out/(name+'.zip')).write_bytes(data)
# Overlapping data spans, missing/truncated ZIP64 values, and local-only encryption must fail before release.
second=raw.index(b'PK\x01\x02',cd+4)
first_name_len=S.unpack_from('<H',raw,cd+28)[0]
first_data=30+first_name_len
second_offset=S.unpack_from('<I',raw,second+42)[0]
data=bytearray(raw)
S.pack_into('<I',data,cd+20,second_offset+30-first_data)
S.pack_into('<I',data,cd+24,second_offset+30-first_data)
(out/'overlap.zip').write_bytes(data)
data=bytearray(raw);S.pack_into('<H',data,6,1);(out/'local-encrypted.zip').write_bytes(data)
# A fake EOCD signature inside a comment must not hide the real end record.
data=bytearray(raw);comment=b'hello PK\x05\x06' + bytes(30);S.pack_into('<H',data,len(raw)-2,len(comment));data.extend(comment)
(out/'comment.zip').write_bytes(data)
(out/'truncated.zip').write_bytes(raw[:len(raw)//2])
data=bytearray((out/'deflate.zip').read_bytes());p=0
while (p:=data.find(b'PK\x01\x02',p))>=0:
    if data[p+46:p+62]==b'content/test.bin': S.pack_into('<I',data,p+24,1);break
    p+=4
(out/'bomb.zip').write_bytes(data)
data=bytearray(raw); pos=raw.index(bytes(range(256))*10);data[pos+10]^=1;(out/'crc.zip').write_bytes(data)
with Z.ZipFile(out/'entry-limit.zip','w',compression=Z.ZIP_STORED) as z:
    for i in range(200001): z.writestr(f'x/{i}',b'')
# Explicit ZIP64 end record and offset extra, with 32-bit data descriptors (Finder regression).
with (out/'sparse.zip').open('wb') as stream:
    stream.seek((1<<32)+4096)
    records=[]
    import zlib
    for path,data in tree().items():
        name=path.encode();offset=stream.tell();crc=zlib.crc32(data)
        stream.write(S.pack('<I5H3I2H',0x04034b50,20,8,0,0,0,0,0,0,len(name),0)+name+data)
        stream.write(S.pack('<4I',0x08074b50,crc,len(data),len(data)))
        extra=S.pack('<HHQ',1,8,offset)
        records.append(S.pack('<I6H3I5H2I',0x02014b50,45,45,8,0,0,0,crc,len(data),len(data),len(name),len(extra),0,0,0,0,0xffffffff)+name+extra)
    offset=stream.tell();directory=b''.join(records);stream.write(directory);end=stream.tell()
    stream.write(S.pack('<IQ2H2I4Q',0x06064b50,44,45,45,0,0,len(records),len(records),len(directory),offset))
    stream.write(S.pack('<IIQI',0x07064b50,0,end,1))
    stream.write(S.pack('<I4H2IH',0x06054b50,0,0,65535,65535,len(directory),0xffffffff,0))
# CP437 names must be decoded identically from local and central headers.
data=raw.replace(b'code/',b'c\x82de/');(out/'cp437.zip').write_bytes(data)
# Real zip(1) forced ZIP64 end/extra records, as requested in 7.11.
force=out/'force';force.mkdir();(force/'test').write_bytes(b'zip64')
subprocess.run(['/usr/bin/zip','-q','-fz',str(out/'forced64.zip'),'test'],cwd=force,check=True)
# Enough large entries to cross checkpoints and observe incremental hole punching.
f=tree()
for i in range(130): f[f'content/large-{i:03}.bin']=bytes([i])*65536
archive('release',f)
archive('release-dlc',f,extras=[('aoc/content/dlc.bin',bytes([0xdc])*65536)])
f=tree();f['content/Bonus/meta/meta.xml']=b'<menu><title_id>0005000C101C9300</title_id></menu>';f['content/Bonus/dlc.bin']=b'dlc';archive('dlc-meta',f)
manifest={'titleID':'00050000101C9300','files':[dict(path=p,size=len(d),sha256=hashlib.sha256(d).hexdigest()) for p,d in tree().items()]}
(out/'manifest').mkdir();(out/'manifest/00050000101C9300.json').write_text(json.dumps(manifest))
print('Fixtures: stored, deflate, forced ZIP64, sparse >4 GiB + descriptors, regional and hostile archives')

# Review regressions: non-game notes, and add-on metadata whose ID is not inferred from a guessed prefix.
archive('notes', prefix='g/', extras=[('my-notes/readme.txt',b'keep me')])
f=tree();f['content/test.bin']=b'wrong size';archive('manifest-size',f)
for name,parent in [('unknown-addon',''),('parent-addon','<parent_title_id>00050000101C9300</parent_title_id>')]:
    f=tree();f['content/Bonus/meta/meta.xml']=f'<menu><title_id>0005000C101D6000</title_id>{parent}</menu>'.encode();f['content/Bonus/dlc.bin']=b'keep DLC'
    archive(name,f)
