"""Relocatable tool settings for Lotus's private IKOS API-spec helpers.

The IKOS analyzer itself is optional and is resolved only when requested.
"""
import os
import argparse
from pathlib import Path
import shutil
import subprocess

VERSION = '3.5 (Lotus API-spec integration)'
SRC_ROOT = str(Path(__file__).resolve().parents[2])
INCLUDE_DIR = str(Path(SRC_ROOT) / 'include')
BIN_DIR = os.environ.get('LOTUS_IKOS_BIN', '')
PREFIX = str(Path(BIN_DIR).parent) if BIN_DIR else ''
BUILD_MODE = os.environ.get('LOTUS_IKOS_BUILD_MODE', 'Release')
HAS_APRON = os.environ.get('LOTUS_IKOS_HAS_APRON') == 'ON'


def _llvm_bin_dir():
    override = os.environ.get('LOTUS_IKOS_LLVM_BIN')
    if override:
        return os.path.abspath(os.path.expanduser(override))
    for name in ('llvm-config-14', 'llvm-config'):
        config = shutil.which(name)
        if config:
            try:
                version = subprocess.check_output(
                    [config, '--version'], text=True).strip()
                if version.split('.')[0] == '14':
                    return subprocess.check_output(
                        [config, '--bindir'], text=True).strip()
            except (OSError, subprocess.CalledProcessError):
                pass
    return ''


LLVM_BIN_DIR = _llvm_bin_dir()


def _llvm_tool(name):
    if LLVM_BIN_DIR:
        path = os.path.join(LLVM_BIN_DIR, name)
    else:
        path = shutil.which(name + '-14')
    if not path or not os.path.isfile(path) or not os.access(path, os.X_OK):
        raise RuntimeError('Cannot find LLVM 14 %s; set '
                           'LOTUS_IKOS_LLVM_BIN to its bin directory' % name)
    return path


def clang():
    return _llvm_tool('clang')


def opt():
    return _llvm_tool('opt')


def llvm_link():
    return _llvm_tool('llvm-link')


def clangxx():
    return _llvm_tool('clang++')


def llvm_objdump():
    return _llvm_tool('llvm-objdump')


def llvm_objcopy():
    return _llvm_tool('llvm-objcopy')


def _ikos_tool(name):
    path = os.path.join(BIN_DIR, name) if BIN_DIR else shutil.which(name)
    if not path or not os.path.isfile(path) or not os.access(path, os.X_OK):
        raise RuntimeError('Cannot find %s; build the lotus-ikos target or set '
                           'LOTUS_IKOS_BIN to the analyzer bin directory' % name)
    return path


def ikos_pp():
    return _ikos_tool('ikos-pp')


def ikos_analyzer():
    return _ikos_tool('ikos-analyzer')


def ikos():
    prefix = os.environ.get('IKOS_PREFIX')
    path = os.path.join(prefix, 'bin', 'ikos') if prefix else None
    if not prefix:
        local = os.path.join(BIN_DIR, 'ikos') if BIN_DIR else ''
        path = local if os.path.isfile(local) else shutil.which('ikos')
    if not path or not os.path.isfile(path) or not os.access(path, os.X_OK):
        raise RuntimeError('IKOS validation requires the matching ikos-api-spec '
                           'analyzer fork; set IKOS_PREFIX or put ikos on PATH')
    return path


def main(argv):
    parser = argparse.ArgumentParser(description='IKOS configuration in Lotus')
    values = {'version': VERSION, 'build-mode': BUILD_MODE, 'prefix': PREFIX,
              'src-root': SRC_ROOT, 'bindir': BIN_DIR, 'includedir': INCLUDE_DIR,
              'has-apron': HAS_APRON, 'llvm-bindir': LLVM_BIN_DIR,
              'llvm-version': '14', 'llvm-prefix': str(Path(LLVM_BIN_DIR).parent),
              'llvm-config': os.path.join(LLVM_BIN_DIR, 'llvm-config'),
              'clang': os.path.join(LLVM_BIN_DIR, 'clang')}
    for name in values:
        parser.add_argument('--' + name, action='store_true')
    options = parser.parse_args(argv[1:])
    for name, value in values.items():
        if getattr(options, name.replace('-', '_')):
            print(value)
