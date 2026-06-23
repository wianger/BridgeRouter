import os
import json
import sys

directory = '.'
output_file = 'merged.json'

merged_data = {}

index_counter = 0

linux_dir = sys.argv[1]

def get_func_configs(func):
    configs = []
    print('[F] ', func)
    func += '('

    for dirpath, _, filenames in os.walk(linux_dir):
        for filename in filenames:
            if filename.endswith('.c'):
                c_file_path = os.path.join(dirpath, filename)
                with open(c_file_path, 'r') as c_file:
                    if func in c_file.read():
                        makefile_path = os.path.join(dirpath, 'Makefile')
                        if os.path.exists(makefile_path):
                            if  "samples" in makefile_path or \
                                "scripts" in makefile_path or \
                                "tools" in makefile_path:
                                return ["NOT_KERNEL"]
                            with open(makefile_path, 'r') as makefile:
                                lines = makefile.readlines()

                            obj_file_name = filename.replace('.c', '.o')
                            while obj_file_name != "":
                                print(f"[o] {obj_file_name} {makefile_path}")
                                for i in range(len(lines)):
                                    if obj_file_name in lines[i] and \
                                        "CFLAG" not in lines[i]:
                                        break
                                if obj_file_name not in lines[i]:
                                    break
                                obj_file_name = ""
                                while i >= 0:
                                    if "obj-y" in lines[i]:
                                        obj_file_name = ""
                                        break
                                    if "-y " in lines[i]:
                                        obj_file_name = lines[i].split('-y ')[0]+".o"
                                        break
                                    if "-objs" in lines[i]:
                                        obj_file_name = lines[i].split('-objs')[0]+".o"
                                        break
                                    if "-$(CONFIG" in lines[i] or "-${CONFIG" in lines[i]:
                                        obj_file_name = ""
                                        print("[i]", lines[i].strip())
                                        if "-$(" in lines[i]:
                                            config = lines[i].split("-$(")[1].split(')')[0]
                                        elif "-${" in lines[i]:
                                            config = lines[i].split("-${")[1].split(')')[0]
                                        configs.append(config)
                                        print(f"[+] config: {config}\n")
                                        break
                                    i -= 1
                            
                        else:
                            print(f"No Makefile found in directory: {dirpath}")
    return list(set(configs))

def generate_config(copy_list):
    configs = []
    for func in copy_list:
        configs += get_func_configs(func)
    return list(set(configs))

def unique_list(items):
    return list(dict.fromkeys(items))

for filename in os.listdir(directory):
    if filename.endswith('.json') and filename.startswith('struct'):
        file_path = os.path.join(directory, filename)
        with open(file_path, 'r') as file:
            data = json.load(file)
            for key, value in data.items():
                if key not in merged_data:
                    value['index'] = index_counter
                    index_counter += 1
                    # if 'copy' in value:
                    #     value['configs'] = generate_config(value['copy'])
                    # else:
                    #     value['configs'] = []
                    if 'bridge_off' in value:
                        value['bridge_off'] = unique_list(value['bridge_off'])
                    if 'router_off' in value:
                        value['router_off'] = unique_list(value['router_off'])
                    if 'skbuffer_off' in value:
                        value['skbuffer_off'] = unique_list(value['skbuffer_off'])
                    if 'skbuffer_read' in value:
                        value['skbuffer_read'] = unique_list(value['skbuffer_read'])
                    if 'skbuffer_write' in value:
                        value['skbuffer_write'] = unique_list(value['skbuffer_write'])
                    if 'skbuffer_alloc' in value:
                        value['skbuffer_alloc'] = unique_list(value['skbuffer_alloc'])
                    if 'skbuffer_size' in value:
                        value['skbuffer_size'] = unique_list(value['skbuffer_size'])
                    merged_data[key] = value
                else:
                    current = merged_data[key]
                    for list_key in ('copy', 'alloc', 'bridge_off', 'router_off',
                                     'skbuffer_off', 'skbuffer_read',
                                     'skbuffer_write', 'skbuffer_alloc',
                                     'skbuffer_size'):
                        if list_key in value:
                            current[list_key] = unique_list(
                                current.get(list_key, []) + value.get(list_key, []))
                    for bool_key in ('bridge', 'router', 'skbuffer'):
                        if bool_key in value:
                            current[bool_key] = current.get(bool_key, False) or value.get(bool_key, False)

with open(output_file, 'w') as file:
    json.dump(merged_data, file, indent=4)

print(f'All JSON files have been merged into {output_file}')
