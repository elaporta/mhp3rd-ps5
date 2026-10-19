#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw an original geometric launcher icon locally, without game artwork or bitmap inputs."""
import binascii
import pathlib
import struct
import zlib


def png():
    size=512
    pixels=bytearray(size*size*4)
    for y in range(size):
        for x in range(size):
            at=(y*size+x)*4
            light=max(0,1-abs(x-256)/360-abs(y-180)/600)
            pixels[at:at+4]=bytes((int(12+12*light),int(31+35*light),int(24+24*light),255))
    def rect(x,y,w,h,color):
        for row in range(max(0,y),min(size,y+h)):
            for col in range(max(0,x),min(size,x+w)):
                at=(row*size+col)*4
                pixels[at:at+4]=bytes((*color,255))
    glyphs={'M':['10001','11011','10101','10101','10001','10001','10001'],
            'H':['10001','10001','10001','11111','10001','10001','10001'],
            'P':['11110','10001','10001','11110','10000','10000','10000'],
            '3':['11110','00001','00001','01110','00001','00001','11110'],
            'r':['00000','00000','10110','11001','10000','10000','10000'],
            'd':['00001','00001','01101','10011','10001','10011','01101']}
    # An abstract mountain/forest silhouette, independently drawn from simple geometry.
    for x in range(64,448):
        peak=180+abs(x-256)//2
        rect(x,peak,1,150,(19,56,37))
    for x,height in ((100,70),(156,92),(356,92),(412,70)):
        for y in range(height):
            half=y//3
            rect(x-half,150+y,2*half+1,1,(38,91,58))
    scale=11; text='MHP3rd'; width=(len(text)*6-1)*scale
    left=(size-width)//2
    for i,char in enumerate(text):
        for y,row in enumerate(glyphs[char]):
            for x,bit in enumerate(row):
                if bit=='1':rect(left+(i*6+x)*scale,290+y*scale,scale,scale,(235,242,209))
    raw=b''.join(b'\0'+pixels[y*size*4:(y+1)*size*4] for y in range(size))
    def chunk(tag,data):
        return struct.pack('>I',len(data))+tag+data+struct.pack('>I',binascii.crc32(tag+data)&0xffffffff)
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',size,size,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b'')
