#!/usr/bin/env python3
#
# This file is part of libFirm.
# Copyright (C) 2012 Karlsruhe Institute of Technology.
import sys
# don't clutter our filesystem with .pyc files...
sys.dont_write_bytecode = True
import argparse
from jinja2 import Environment
import filters
import jinjautil

# Since python3, importing an arbitrary file became somewhat more involved. This piece of code is from "spack" at
# https://github.com/epfl-scitas/spack/blob/releases/humagne/lib/spack/llnl/util/lang.py
# and licensed under Apache-2.0.
def load_module_from_file(module_name, module_path):
    if sys.version_info[0] == 3 and sys.version_info[1] >= 5:
        import importlib.util
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
    else:
        import importlib.machinery
        loader = importlib.machinery.SourceFileLoader(module_name, module_path)
        module = loader.load_module()

    return module


def main(argv):
    description = 'Generate code/docu from node specification'
    parser = argparse.ArgumentParser(add_help=True, description=description)
    parser.add_argument('-I', dest='includedirs', action='append',
                        help='directories for templates/python modules',
                        default=[], metavar='DIR')
    parser.add_argument('-D', dest='definitions', action='append',
                        help='definition exported to jinja',
                        default=[], metavar='NAME=DEF')
    parser.add_argument('-e', dest='extra', action='append',
                        help='load extra specification/filters',
                        default=[])
    parser.add_argument('specfile', action='store',
                        help='node specification file')
    parser.add_argument('templatefile', action='store',
                        help='jinja2 template file')
    config = parser.parse_args()

    # Append includedirs to python path and template loader searchpath
    for dir in config.includedirs:
        sys.path.insert(1, dir)

    loader = jinjautil.SimpleLoader()
    loader.includedirs += config.includedirs

    # Load specfile
    load_module_from_file('spec', config.specfile)
    for num, extrafile in enumerate(config.extra):
        load_module_from_file('extra%s' % (num,), extrafile)

    env = Environment(loader=loader, keep_trailing_newline=True)
    env.globals.update(jinjautil.exports)
    env.filters.update(jinjautil.filters)
    for definition in config.definitions:
        (name, _, replacement) = definition.partition("=")
        env.globals[name] = replacement

    template = env.get_template(config.templatefile)
    result = template.render()
    sys.stdout.write(result)


if __name__ == "__main__":
    main(sys.argv)
