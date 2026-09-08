#!/usr/bin/python3

import os
import shutil
import subprocess
import tempfile
import sys
import argparse

parser = argparse.ArgumentParser(description = 'Generate a managarm initrd')
parser.add_argument('-t', '--triple', dest = 'arch',
		choices = ['x86_64-managarm', 'aarch64-managarm', 'riscv64-managarm'], default = 'x86_64-managarm',
		help = 'Target system triple (default: x86_64-managarm)')
parser.add_argument('--sysroot', type=str, default='/')
parser.add_argument('-o', '--out', type=str, default='initrd.cpio')

args = parser.parse_args()

class Entry:
	__slots__ = ('is_dir', 'source')

	def __init__(self, is_dir=False, source=None):
		self.is_dir = is_dir
		self.source = source

file_dict = dict()

def add_dir(rel_path):
	file_dict[rel_path] = Entry(is_dir=True)

def add_file(src_prefix, tree_prefix, filename):
	src_path = os.path.join(src_prefix, filename)
	rel_path = os.path.join(tree_prefix, filename)
	file_dict[rel_path] = Entry(source=src_path)

# Add all the files.

add_dir('lib')
add_dir('usr')
add_dir('usr/bin')
add_dir('usr/lib')
if args.arch != 'riscv64-managarm':
	add_dir('usr/lib64')
add_dir('usr/lib/managarm')
add_dir('usr/lib/managarm/server')

# The kernel
add_file('usr/managarm/bin', '', 'thor')

# Runtime libraries.
add_file('lib', 'lib', args.arch + '-mlibc-ld.so')
add_file('usr/lib', 'usr/lib', 'libhelix.so')
add_file('usr/lib', 'usr/lib', 'libc.so')
add_file('usr/lib', 'usr/lib', 'libm.so')
add_file('usr/lib', 'usr/lib', 'liblewis.so')
add_file('usr/lib', 'usr/lib', 'libz.so.1')
add_file('usr/lib', 'usr/lib', 'libvirtio_core.so')
add_file('usr/lib', 'usr/lib', 'libarch.so')
add_file('usr/lib', 'usr/lib', 'libfs_protocol.so')
add_file('usr/lib', 'usr/lib', 'libhw_protocol.so')
add_file('usr/lib', 'usr/lib', 'libkernlet_protocol.so')
add_file('usr/lib', 'usr/lib', 'libmbus.so')
add_file('usr/lib', 'usr/lib', 'libostrace_protocol.so')
add_file('usr/lib', 'usr/lib', 'libsvrctl_protocol.so')
add_file('usr/lib', 'usr/lib', 'libusb_protocol.so')
if args.arch == 'riscv64-managarm':
	add_file('usr/lib', 'usr/lib', 'libgcc_s.so.1')
	add_file('usr/lib', 'usr/lib', 'libstdc++.so.6')
else:
	add_file('usr/lib64', 'usr/lib64', 'libgcc_s.so.1')
	add_file('usr/lib64', 'usr/lib64', 'libstdc++.so.6')

# User-space core components.
add_file('usr/bin', 'usr/bin', 'sif')
add_file('usr/bin', 'usr/bin', 'mbus')
add_file('usr/bin', 'usr/bin', 'kernletcc')
add_file('usr/bin', 'usr/bin', 'posix-devserver')
add_file('usr/bin', 'usr/bin', 'clocktracker')
add_file('usr/bin', 'usr/bin', 'posix-subsystem')
add_file('usr/bin', 'usr/bin', 'posix-init')

# Essential drivers.
add_file('usr/lib', 'usr/lib', 'libblockfs.so')
add_file('usr/lib', 'usr/lib', 'libsound.so')
add_file('usr/bin', 'usr/bin', 'ehci')
add_file('usr/bin', 'usr/bin', 'xhci')
add_file('usr/bin', 'usr/bin', 'netserver')
add_file('usr/bin', 'usr/bin', 'block-ahci')
add_file('usr/bin', 'usr/bin', 'block-nvme')
add_file('usr/bin', 'usr/bin', 'storage')
add_file('usr/bin', 'usr/bin', 'virtio-block')
add_file('usr/bin', 'usr/bin', 'virtio-console')
add_file('usr/bin', 'usr/bin', 'dmalog')
add_file('usr/bin', 'usr/bin', 'snd-hda')

if args.arch == 'x86_64-managarm':
	add_file('usr/bin', 'usr/bin', 'uhci')
	add_file('usr/bin', 'usr/bin', 'block-ata')
	add_file('usr/bin', 'usr/bin', 'uart')

# Essential utilities.
add_file('usr/bin', 'usr/bin', 'runsvr')

# Server descriptions.
for fname in os.listdir(os.path.join(args.sysroot, 'usr/lib/managarm/server')):
	if not fname.endswith('.bin'):
		continue
	add_file('usr/lib/managarm/server', 'usr/lib/managarm/server', fname)

# Copy (= hard link) the files to a temporary directory, run GNU cpio.

tree_path = tempfile.mkdtemp(prefix='initrd-', dir='.')

file_list = sorted(file_dict.keys())
for rel_path in file_list:
	entry = file_dict[rel_path]
	dest_path = os.path.join(tree_path, rel_path)

	if entry.is_dir:
		os.mkdir(dest_path)
	else:
		os.link(os.path.realpath(os.path.join(args.sysroot, entry.source)), dest_path)

proc = subprocess.Popen(['cpio', '--create', '--format=newc',
			'-D', tree_path,
			'--file', args.out,
			'--quiet'],
		stdin=subprocess.PIPE,
		encoding='ascii')
proc.communicate(input='\n'.join(file_list))
if proc.returncode != 0:
	sys.exit(1)

shutil.rmtree(tree_path)
