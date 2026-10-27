#!/usr/bin/env python3
"""Builds data/java_to_bedrock.tsv.gz, the Java -> Bedrock block table built into BE-Matica.

Inputs come from PrismarineJS/minecraft-data (MIT), data/bedrock/<version>/:
  blocksJ2B.json    Java block state -> Bedrock block state
  blockStates.json  Bedrock states with their NBT types and the block version number

Usage: make_block_map.py blocksJ2B.json blockStates.json data/java_to_bedrock.tsv.gz

Each output line is: java_name[props]<TAB>bedrock_name<TAB>key=Tvalue,... where T is b (byte),
i (int) or s (string). The first line is the Bedrock block version that structures store.
"""
import gzip
import json
import sys


def parse_state(s):
    name, _, rest = s.partition("[")
    props = [p.split("=", 1) for p in rest.rstrip("]").split(",") if p]
    return name, props


def main(j2b_path, states_path, out_path):
    j2b = json.load(open(j2b_path))
    types, versions = {}, set()
    for block in json.load(open(states_path)):
        versions.add(block["version"])
        for key, st in block["states"].items():
            types[("minecraft:" + block["name"], key)] = st["type"]
    if len(versions) != 1:
        sys.exit(f"expected one block version, found {sorted(versions)}")
    code = {"byte": "b", "int": "i", "string": "s"}
    lines = [str(versions.pop())]
    for java, bedrock in sorted(j2b.items()):
        name, props = parse_state(bedrock)
        states = []
        for key, value in props:
            t = types.get((name, key))
            if t is None:
                sys.exit(f"no type for {name} {key}")
            if t == "byte":  # minecraft-data writes Bedrock bits as true/false
                value = {"true": "1", "false": "0"}.get(value, value)
            states.append(f"{key}={code[t]}{value}")
        lines.append(f"{java}\t{name}\t{','.join(states)}")
    with gzip.GzipFile(out_path, "wb", mtime=0) as f:
        f.write(("\n".join(lines) + "\n").encode())


if __name__ == "__main__":
    main(*sys.argv[1:])
