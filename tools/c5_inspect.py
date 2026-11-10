#!/usr/bin/env python3
"""Offline inspection of a verified C5 backup; never opens a hardware port."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import struct
import sys
import zlib

import meoave
from esptool.bin_image import ESP32C5FirmwareImage


def inspect_image(data, offset, size):
    raw=data[offset:offset+size]
    if not raw or raw[:24]==b'\xff'*24:
        return {'present':False,'offset':offset}
    image=ESP32C5FirmwareImage(io.BytesIO(raw))
    checksum=image.checksum==image.calculate_checksum()
    digest=not image.append_digest or image.stored_digest==image.calc_digest
    if not checksum or not digest or image.chip_id!=23:
        raise ValueError(f'Invalid C5 image at {offset:#x}')
    result={'present':True,'offset':offset,'checksum_ok':checksum,'digest_ok':digest,
        'chip_id':image.chip_id,'min_rev_full':image.min_rev_full,'max_rev_full':image.max_rev_full,
        'flash_mode':image.flash_mode,'flash_size_freq':image.flash_size_freq,
        'image_size':image.data_length+32 if image.append_digest else None}
    if raw[32:36]==struct.pack('<I',0xabcd5432):
        def text(start,length): return raw[start:start+length].split(b'\0',1)[0].decode('utf-8',errors='replace')
        result.update(version=text(48,32),project=text(80,32),build_time=text(112,16),
            build_date=text(128,16),idf=text(144,32),elf_sha=raw[176:208].hex())
    return result


def inspect_backup(data):
    if len(data)!=0x400000:
        raise ValueError('A complete 4 MB backup is required')
    idf=Path(meoave.environment(meoave.load_config())['IDF_PATH'])
    sys.path.insert(0,str(idf/'components/partition_table'))
    import gen_esp32part as partition
    table=partition.PartitionTable.from_binary(data[0xc000:0xd000])
    table.verify(); table.verify_size_fits(len(data))
    result={'partition_table_valid':True,'partitions':[
        {'name':p.name,'type':p.type,'subtype':p.subtype,'offset':p.offset,'size':p.size} for p in table],
        'bootloader':inspect_image(data,0x2000,0xa000),'applications':{},'otadata':[]}
    for p in table:
        if p.type==0:
            result['applications'][p.name]=inspect_image(data,p.offset,p.size)
        if p.type==1 and p.subtype==0:
            for index in (0,1):
                o=p.offset+index*4096
                seq,_,state,crc=struct.unpack_from('<I20sII',data,o)
                valid=seq!=0xffffffff and state not in (3,4) and crc==zlib.crc32(data[o:o+4],0xffffffff)
                result['otadata'].append({'copy':index,'offset':o,'seq':seq,'state':state,
                    'crc':crc,'valid':valid,'erased':data[o:o+32]==b'\xff'*32})
    valid=[x for x in result['otadata'] if x['valid']]
    slots=[p for p in table if p.type==0 and 0x10<=p.subtype<=0x1f]
    factory=next((p.name for p in table if p.type==0 and p.subtype==0),None)
    if valid and slots:
        active=max(valid,key=lambda x:x['seq'])
        subtype=0x10+(active['seq']-1)%len(slots)
        selected=next((p.name for p in slots if p.subtype==subtype),None)
        reason='highest valid otadata sequence (offline prediction)'
    else:
        selected=factory or next((p.name for p in slots if p.subtype==0x10),None)
        reason='no valid otadata: factory/ota_0 fallback (offline prediction)'
    result['predicted_boot']={'partition':selected,'reason':reason,'runtime_verified':False}
    if not result['bootloader']['present'] or not result['applications'].get(selected,{}).get('present'):
        raise ValueError('Predicted boot image missing')
    return result


def main(argv=None):
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--backup-report',type=Path,required=True)
    p.add_argument('--historical-report',type=Path)
    p.add_argument('--facts-report',type=Path)
    p.add_argument('--report',type=Path,required=True)
    args=p.parse_args(argv)
    r={'ok':False,'scope':'offline Flash integrity/configuration; not application runtime proof'}
    try:
        source=json.loads(args.backup_report.read_text(encoding='utf-8'))
        if not source.get('ok') or not source.get('completed') or not source.get('full_flash'):
            raise ValueError('Backup report is incomplete/unverified')
        data=Path(source['output']).read_bytes()
        if hashlib.sha256(data).hexdigest()!=source['sha256']:
            raise ValueError('Backup SHA-256 differs from hardware report')
        r.update(inspect_backup(data))
        r['backup_sha256']=source['sha256']
        if args.historical_report:
            historical=json.loads(args.historical_report.read_text(encoding='utf-8'))
            if not historical.get('ok') or not historical.get('flash',{}).get('verified'):
                raise ValueError('Historical flash evidence not verified')
            boot=r['bootloader']; app=r['applications']['ota_0']
            if boot['image_size'] is None or app['image_size'] is None:
                raise ValueError('Missing image length for historical reconstruction')
            # Exact original four-region C5 provisioning layout. Non-image NVS/
            # factory bytes are not part of that historical merged payload.
            rebuilt=bytearray(b'\xff'*(app['offset']+app['image_size']))
            regions=[(boot['offset'],boot['image_size']),(0xc000,3072),(0x16000,8192),
                     (app['offset'],app['image_size'])]
            for o,n in regions: rebuilt[o:o+n]=data[o:o+n]
            digest=hashlib.sha256(rebuilt).hexdigest()
            r['historical_payload']={'reconstructed_sha256':digest,
                'recorded_sha256':historical['payload_sha256'],
                'matches':digest==historical['payload_sha256'],'regions':regions}
        if args.facts_report:
            facts=json.loads(args.facts_report.read_text(encoding='utf-8'))
            if not facts.get('ok') or 'facts' not in facts:
                raise ValueError('Target revision facts not verified')
            f=facts['facts']; revision=f['revision_full']
            app=r['applications'][r['predicted_boot']['partition']]
            r['chip_revision']={'full':revision,'major':f['revision_major'],'minor':f['revision_minor'],
                'compatible_with_images':revision>=r['bootloader']['min_rev_full'] and
                    revision>=app['min_rev_full'] and (app['max_rev_full'] in (0,65535) or revision<=app['max_rev_full'])}
        r['ok']=True
        print(json.dumps(r,ensure_ascii=False,indent=2))
        return 0
    except Exception as exc:
        r['error']=f'{type(exc).__name__}: {exc}'
        print(r['error'])
        return 1
    finally:
        args.report.parent.mkdir(parents=True,exist_ok=True)
        args.report.write_text(json.dumps(r,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')


if __name__=='__main__': raise SystemExit(main())
