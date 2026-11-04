"""Small bounded FDT reader used to verify imported kernel-owned pin bindings."""
import struct
from profiles import ProfileError

def properties(data):
    if len(data)<40:raise ProfileError('Truncated device tree')
    magic,total,structure,strings,_,version,_,_,strings_size,structure_size=struct.unpack_from('>10I',data)
    if magic!=0xd00dfeed or version<16 or total>len(data) or structure+structure_size>total or strings+strings_size>total:raise ProfileError('Invalid device tree header')
    pos=structure;end=structure+structure_size;stack=[];result={}
    while pos+4<=end:
        token=struct.unpack_from('>I',data,pos)[0];pos+=4
        if token==1:
            stop=data.find(b'\0',pos,end)
            if stop<0:raise ProfileError('Unterminated FDT node')
            stack.append(data[pos:stop].decode('ascii'));pos=(stop+4)&~3
            result['/'+ '/'.join(stack[1:])]={}
        elif token==2:
            if not stack:raise ProfileError('Unbalanced FDT nodes')
            stack.pop()
        elif token==3:
            if pos+8>end or not stack:raise ProfileError('Truncated FDT property')
            size,offset=struct.unpack_from('>II',data,pos);pos+=8
            if pos+size>end or offset>=strings_size:raise ProfileError('FDT property out of bounds')
            stop=data.find(b'\0',strings+offset,strings+strings_size)
            if stop<0:raise ProfileError('Unterminated FDT property name')
            name=data[strings+offset:stop].decode('ascii')
            result['/'+ '/'.join(stack[1:])][name]=data[pos:pos+size];pos=(pos+size+3)&~3
        elif token==4:continue
        elif token==9:
            if stack:raise ProfileError('Unclosed FDT nodes')
            return result
        else:raise ProfileError('Unknown FDT token')
    raise ProfileError('Device tree has no end token')

def cells(value):
    if len(value)%4:raise ProfileError('Misaligned FDT cells')
    return list(struct.unpack('>'+str(len(value)//4)+'I',value))
