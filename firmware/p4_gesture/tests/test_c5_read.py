"""Read-only backup must reject stale, incomplete or corrupt evidence."""
from pathlib import Path
import hashlib
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools'))
import c5_read as read

TOKEN='123456789abc'


class Stream:
    def __init__(self,chunks):
        self.chunks=list(chunks); self.writes=[]; self.now=0
    def write(self,data): self.writes.append(data)
    def flush(self): pass
    def read(self,count):
        self.now+=.01
        return self.chunks.pop(0) if self.chunks else b''


def done(data,operation=TOKEN,offset=0x2000):
    return (f'MEO:C5_READ token={TOKEN} operation={operation} state=DONE '
            f'offset=0x{offset:08x} size={len(data)} error=0 md5={hashlib.md5(data).hexdigest()}\n').encode()


def parts(data):
    return [(f'MEO:C5_DATA token={TOKEN} pos={pos} size={len(data[pos:pos+128])} '
             f'data={data[pos:pos+128].hex()}\n').encode() for pos in range(0,len(data),128)]


class ReadTests(unittest.TestCase):
    def run_block(self,stream,size=256):
        with patch.object(read.secrets,'token_hex',return_value=TOKEN):
            return read.Reader(stream,clock=lambda:stream.now).block(0x2000,size)

    def test_read_once_with_split_status_and_digest(self):
        data=bytes(range(256)); status=done(data)
        stream=Stream([status[:40],status[40:],*parts(data)])
        self.assertEqual(self.run_block(stream),data)
        self.assertEqual(sum(x.startswith(b'C5 READ ') for x in stream.writes),1)
        self.assertFalse(any(b'FLASH ' in x or b'RESET' in x for x in stream.writes))

    def test_stale_operation_ignored(self):
        data=b'a'*256; stream=Stream([done(data,operation='old'),done(data),*parts(data)])
        self.assertEqual(self.run_block(stream),data)

    def test_changed_address_rejected(self):
        stream=Stream([done(b'a'*256,offset=0x3000)])
        with self.assertRaisesRegex(ValueError,'address'):
            self.run_block(stream)

    def test_corrupt_data_rejected(self):
        stream=Stream([done(b'a'*256),*parts(b'b'*256)])
        with self.assertRaisesRegex(ValueError,'MD5'):
            self.run_block(stream)

    def test_read_rejection_no_repeat(self):
        stream=Stream([f'MEO:C5_READ_ACCEPT token={TOKEN} accepted=0\n'.encode()])
        with self.assertRaisesRegex(RuntimeError,'rejected'):
            self.run_block(stream)
        self.assertEqual(sum(x.startswith(b'C5 READ ') for x in stream.writes),1)

    def test_missing_data_stops_without_new_read(self):
        stream=Stream([done(b'a'*256)])
        with self.assertRaises(TimeoutError): self.run_block(stream)
        self.assertEqual(sum(x.startswith(b'C5 READ ') for x in stream.writes),1)
        self.assertEqual(sum(x.startswith(b'C5 DATA ') for x in stream.writes),3)

    def test_zero_length_cached_piece_rejected(self):
        stream=Stream([done(b'a'*256),f'MEO:C5_DATA token={TOKEN} pos=0 size=0 data=\n'.encode()])
        with self.assertRaisesRegex(ValueError,'length'): self.run_block(stream)

    def test_bounds_before_usb(self):
        for offset,size in ((-1,1),(0,0),(0,4097),(0x400000,1),(0x3fffff,2)):
            with self.subTest(offset=offset,size=size):
                stream=Stream([])
                with self.assertRaises(ValueError): read.Reader(stream).block(offset,size)
                self.assertEqual(stream.writes,[])

    def test_entry_point_does_not_add_flash(self):
        with patch.object(sys,'argv',['meoave.py','c5-read','--output','backup.bin',
                '--report','report.json','--limit','0x20000']), \
             patch.object(read.meoave.subprocess,'run') as run:
            run.return_value.returncode=0
            self.assertEqual(read.meoave.main(),0)
        command=run.call_args.args[0]
        self.assertNotIn('--flash',command)
        self.assertNotIn('--install',command)
        self.assertEqual(command[command.index('--limit')+1],'0x20000')


if __name__=='__main__': unittest.main()
