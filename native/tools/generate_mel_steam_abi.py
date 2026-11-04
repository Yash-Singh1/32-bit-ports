#!/usr/bin/env python3
"""Extend a pre-Mel ABI table using local SDK 1.40/1.41 reference inputs.

Usage: generate_mel_steam_abi.py SDK_DIRECTORY BASELINE_INC OUTPUT_INC
See ../MEL.md for provenance and reproduction.
"""
from pathlib import Path
import argparse
import json, re, sys, subprocess, tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("sdk_directory", type=Path)
parser.add_argument("baseline_inc", type=Path)
parser.add_argument("output_inc", type=Path)
options = parser.parse_args()
root = Path(__file__).resolve().parents[2]
refs = options.sdk_directory
api=json.loads((refs/'steam_api.json').read_text())
def normalize(value):
 if isinstance(value,dict): return {k: normalize(v) for k,v in value.items()}
 if isinstance(value,list): return [normalize(v) for v in value]
 if isinstance(value,str): return re.sub(r'\b(?:class|struct|enum)\s+', '', value)
 return value
api=normalize(api)
def mac_header(path):
 text=path.read_text(encoding='latin1')
 text=re.sub(r'^\s*#include[^\n]*', '', text, flags=re.M)
 result=subprocess.run(['xcrun','clang','-E','-P','-x','c++','-DPOSIX','-DOSX','-DVALVE_CALLBACK_PACK_SMALL=1','-'],input=text,text=True,capture_output=True,check=True)
 return result.stdout
api['interfaces']=[]
api['callback_structs']=[]
scratch = tempfile.TemporaryDirectory(prefix='mel-steam-abi-')
normalized = Path(scratch.name) / 'normalized.json'
normalized.write_text(json.dumps(api))
prefix=(root/'native/tools/generate_steam_abi.py').read_text().split('\nmethods = []\ninterfaces = []\n')[0]
sys.argv=['generate',str(normalized)]
namespace={};exec(compile(prefix,'steam-abi-types','exec'),namespace)
code=namespace['code']
inc = options.output_inc
s = options.baseline_inc.read_text()
# Rebuild the Mac SteamClient017 facade from SDK 1.41: no PS3-only slot,
# and the ParentalSettings accessor appended without changing the version.
base_block=re.search(r'static const struct steam_method steam_methods\[\] = \{(.*?)\n\};',s,re.S)
base_rows=re.findall(r'^    \{.*\},$',base_block[1],re.M)
client_methods={re.search(r'"([^"\n]+)"',row)[1]:row for row in base_rows[:42]}
legacy_interface=re.search(r'\{"SteamClient017", (\d+), (\d+)\}',s)
first,count=int(legacy_interface[1]),int(legacy_interface[2])
header=mac_header(refs/'isteamclient017141.h')
header=re.sub(r'/\*.*?\*/|//[^\n]*','',header,flags=re.S)
names=re.findall(r'virtual\s+[\w\s*&]+?\b(\w+)\s*\(',header)
names=[name for name in names if name!='GetISteamPS3OverlayRender']
rows=[]
for name in names:
 if name=='SetLocalIPBinding':row='    {"SetLocalIPBinding", \'v\', "iu"},'
 elif name=='GetISteamUnifiedMessages':row='    {"GetISteamUnifiedMessages", \'o\', "iip"},'
 else:row=client_methods.get(name,f'    {{"{name}", \'?\', ""}},')
 rows.append(row)
s=s.replace('\n'.join(base_rows[first:first+count]),'\n'.join(rows),1)
delta=len(rows)-count
def adjust_interface(match):
 version,offset,total=match[1],int(match[2]),int(match[3])
 if version=='SteamClient017':total=len(rows)
 if offset>first:offset+=delta
 return f'{{"{version}", {offset}, {total}}}'
s=re.sub(r'\{"([^"\n]+)", (\d+), (\d+)\}',adjust_interface,s)
method_block=re.search(r'static const struct steam_method steam_methods\[\] = \{(.*?)\n\};',s,re.S)
method_count=len(re.findall(r'^    \{',method_block[1],re.M))
existing=set(re.findall(r'\{"([^"]+)", \d+, \d+\}',s))
new_methods=[];new_interfaces=[]
for header in sorted(refs.glob('isteam*.h')):
 raw_header=header.read_text(encoding='latin1')
 text=mac_header(header)
 versions=re.findall(r'#define\s+\w*INTERFACE_VERSION\w*\s+"([^"]+)"',raw_header)
 if not versions: continue
 version=versions[0]
 if version in existing: continue
 body=re.search(r'class\s+(ISteam\w+)\s*\{(.*?)^\};',text,re.M|re.S)
 assert body,header
 classname=body[1]
 byname={}
 method_api = normalize(json.loads((refs/'steam_api141.json').read_text())) if header.name=='isteamhtmlsurface004.h' else api
 for m in method_api['methods']:
  if m['classname']==classname:byname.setdefault(m['methodname'],[]).append(m)
 names=re.findall(r'virtual\s+[\w\s*&]+?\b(\w+)\s*\(',body[2])
 start=method_count+len(new_methods)
 for name in names:
  if byname.get(name):
   m=byname[name].pop(0)
   ret=code(m['returntype'],True)
   args=''.join(code(p['paramtype'],output_string='out_string' in p) for p in m.get('params',[]))
  else:
   ret,args=('v','') if name in ('RunFrame','DestroyAllInterfaces') else ('?','')
  new_methods.append((name,ret,args))
 new_interfaces.append((version,start,len(names)))
 print(version,len(names))
methods=''.join(f'    {{"{name}", \'{ret}\', "{args}"}},\n' for name,ret,args in new_methods)
interfaces=''.join(f'    {{"{version}", {first}, {count}}},\n' for version,first,count in new_interfaces)
pos=method_block.end()-3
s=s[:pos]+'\n    /* Steamworks SDK 1.40 legacy interfaces used by Portal Stories: Mel. */\n'+methods+s[pos:]
block=re.search(r'static const struct steam_interface steam_interfaces\[\] = \{(.*?)\n\};',s,re.S)
pos=block.end()-3
s=s[:pos]+'\n'+interfaces+s[pos:]
inc.write_text(s)
print('Added',len(new_methods),'methods; unsupported slots:',[(n,r,a) for n,r,a in new_methods if '?' in r+a])

scratch.cleanup()
