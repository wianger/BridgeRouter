import os
import json
import sys

directory = '.'
output_file = 'merged.json'

merged_data = {}

index_counter = 0
site_counter = 0

linux_dir = sys.argv[1]

FUNCTION_FIELDS = ('functions', 'funcs', 'target_funcs', 'targets',
                   'kernel_funcs')

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

def list_value(value):
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]

def normalize_func_list(items):
    funcs = []
    for item in list_value(items):
        if isinstance(item, str) and item:
            funcs.append(item)
    return unique_list(funcs)

def normalize_site_kind(kind):
    if not isinstance(kind, str) or kind == '':
        return 'func'
    if kind in ('function', 'target', 'generic', 'kernel_func'):
        return 'func'
    return kind

def unique_list(items):
    return list(dict.fromkeys(items))

def add_site(sites, seen_sites, kind, func):
    global site_counter

    kind = normalize_site_kind(kind)
    if not isinstance(func, str) or func == '':
        return

    key = (kind, func)
    if key in seen_sites:
        return
    seen_sites.add(key)

    sites.append({
        'id': site_counter,
        'kind': kind,
        'func': func,
    })
    site_counter += 1

def build_sites(value):
    sites = []
    seen_sites = set()
    alloc_list = normalize_func_list(value.get('alloc'))
    copy_list = normalize_func_list(value.get('copy'))
    func_list = []
    existing_sites = list_value(value.get('sites'))

    for field in FUNCTION_FIELDS:
        func_list += normalize_func_list(value.get(field))
    func_list = unique_list(func_list)

    value['alloc'] = alloc_list
    value['copy'] = copy_list
    value['functions'] = func_list
    for field in FUNCTION_FIELDS:
        if field != 'functions':
            value.pop(field, None)

    for func in alloc_list:
        add_site(sites, seen_sites, 'alloc', func)
    for func in copy_list:
        add_site(sites, seen_sites, 'copy', func)
    for func in func_list:
        add_site(sites, seen_sites, 'func', func)
    for site in existing_sites:
        if not isinstance(site, dict):
            continue
        func = site.get('func') or site.get('function') or site.get('name')
        add_site(sites, seen_sites, site.get('kind'), func)

    value['sites'] = sites

for filename in sorted(os.listdir(directory)):
    if filename.endswith('.json') and filename.startswith('struct'):
        file_path = os.path.join(directory, filename)
        with open(file_path, 'r') as file:
            data = json.load(file)
            for key, value in sorted(data.items()):
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
                    build_sites(value)
                    merged_data[key] = value

with open(output_file, 'w') as file:
    json.dump(merged_data, file, indent=4)

print(f'All JSON files have been merged into {output_file}')
