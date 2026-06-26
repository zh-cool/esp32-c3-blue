#!/usr/bin/env python3
"""从 build/compile_commands.json 自动生成 .vscode/c_cpp_properties.json"""
import json, os, sys, re

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_CCJSON = os.path.join(SCRIPT_DIR, "build", "compile_commands.json")

if not os.path.exists(BUILD_CCJSON):
    print(f"错误: 未找到 {BUILD_CCJSON}")
    print("请先运行: idf.py build")
    sys.exit(1)

with open(BUILD_CCJSON) as f:
    data = json.load(f)

# 从 main/blue.c 的编译命令提取 -I 路径和编译器路径
includes = set()
compiler = None

for entry in data:
    if "main/blue.c" in entry.get("file", ""):
        cmd = entry.get("command", "")
        if not cmd:
            continue
        # 提取编译器路径 (第一个词)
        match = re.match(r'(\S+)', cmd)
        if match:
            compiler = match.group(1)
        for word in cmd.split():
            if word.startswith("-I"):
                includes.add(word[2:])
        break

if not includes:
    print("错误: 未在 compile_commands.json 中找到 blue.c 的编译命令")
    sys.exit(1)

sorted_paths = sorted(includes)
includes_json = json.dumps(sorted_paths, indent=16)

# 从编译器路径推断 toolchain 路径
compiler_path = compiler if compiler else "/home/austin/.espressif/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin/riscv32-esp-elf-gcc"

props = f"""{{
    "configurations": [
        {{
            "name": "ESP32-C3",
            "compilerPath": "{compiler_path}",
            "intelliSenseMode": "linux-gcc-arm",
            "cStandard": "c17",
            "defines": [
                "ESP_PLATFORM",
                "CONFIG_BT_ENABLED=1",
                "CONFIG_BT_NIMBLE_ENABLED=1"
            ],
            "includePath": {includes_json}
        }}
    ],
    "version": 4
}}
"""

out_path = os.path.join(SCRIPT_DIR, ".vscode", "c_cpp_properties.json")
os.makedirs(os.path.dirname(out_path), exist_ok=True)
with open(out_path, "w") as f:
    f.write(props)
print(f"已生成: {out_path}")
print(f"共 {len(sorted_paths)} 个包含路径")
