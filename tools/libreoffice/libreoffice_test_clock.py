"""QA only: freeze two wall-clock APIs in one newly-created, owned Win64 child.

No disk runtime patch, OS clock change, field edit, exception swallowing or
process attachment. The parent QA runner owns this launcher and its child tree.
Each DLL load event suspends all child threads; verify exact system DLL bytes,
patch only GetSystemTime[Precise]AsFileTime in child memory, and resume ordinary
debug handling. System DLL disk bytes and other processes remain unchanged.
Missing observations or failures are errors, preserving receipts and outputs.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from ctypes import wintypes as W
from datetime import datetime, timezone
from pathlib import Path

METADATA = Path(__file__).resolve().parents[2] / 'tests/config/libreoffice_test_clock.json'
FUNCTIONS = ('GetSystemTimeAsFileTime', 'GetSystemTimePreciseAsFileTime')
KIND = 'owned-win64-process-wall-clock'

class ExceptionRecord(ctypes.Structure):
    _fields_ = [('code', W.DWORD), ('flags', W.DWORD), ('record', W.LPVOID), ('address', W.LPVOID),
                ('count', W.DWORD), ('info', ctypes.c_size_t * 15)]

class ExceptionInfo(ctypes.Structure):
    _fields_ = [('record', ExceptionRecord), ('first', W.DWORD)]

class LoadDll(ctypes.Structure):
    _fields_ = [('file', W.HANDLE), ('base', W.LPVOID), ('debug_offset', W.DWORD), ('debug_size', W.DWORD),
                ('image_name', W.LPVOID), ('unicode', W.WORD)]

class CreateProcess(ctypes.Structure):
    _fields_ = [('file', W.HANDLE), ('process', W.HANDLE), ('thread', W.HANDLE), ('base', W.LPVOID),
                ('debug_offset', W.DWORD), ('debug_size', W.DWORD), ('thread_local', W.LPVOID),
                ('start', W.LPVOID), ('image_name', W.LPVOID), ('unicode', W.WORD)]

class DebugData(ctypes.Union):
    _fields_ = [('exception', ExceptionInfo), ('dll', LoadDll), ('create', CreateProcess), ('exit', W.DWORD)]

class DebugEvent(ctypes.Structure):
    _fields_ = [('code', W.DWORD), ('pid', W.DWORD), ('tid', W.DWORD), ('data', DebugData)]

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

def verify_platform(unix_time):
    if os.name != 'nt' or ctypes.sizeof(W.LPVOID) != 8 or ctypes.sizeof(DebugEvent) != 176:
        raise ValueError('Requires verified Windows x64 DEBUG_EVENT layout')
    if type(unix_time) is not int or not 0 < unix_time < 0x80000000:
        raise ValueError('Require positive signed-32-bit UNIX seconds')


def reviewed_metadata(source, source_version):
    # Source evidence is explicitly pinned, never inferred from current output.
    from libreoffice_conversion_expectations import pinned_file
    data = json.loads(METADATA.read_text(encoding='utf-8'))
    if (data.get('version') != 1 or data.get('source_version') != source_version
            or data.get('normal_restart_exit') != 81 or data.get('crash_restart_exit') != 79
            or data.get('max_attempts') != 3 or data.get('functions') != list(FUNCTIONS)
            or len(data.get('source_evidence', [])) != 4):
        raise ValueError('Clock metadata contract changed')
    source = Path(source).resolve(strict=True)
    for entry in data['source_evidence']:
        pinned_file(source, entry['path'], entry['sha256'])
    return data


def system_functions(unix_time):
    import pefile  # Development-only dependency; missing support fails preflight.
    system_dll = (Path(os.environ['SystemRoot']) / 'System32/KernelBase.dll').resolve(strict=True)
    system_hash = digest(system_dll)
    image = pefile.PE(str(system_dll))
    if image.FILE_HEADER.Machine != 0x8664:
        raise ValueError('System clock DLL must be AMD64')
    exports = image.DIRECTORY_ENTRY_EXPORT.symbols
    symbols = [next(item for item in exports if item.name == name and item.forwarder is None)
               for name in (item.encode('ascii') for item in FUNCTIONS)]
    filetime = (unix_time + 11644473600) * 10000000
    code = b'\x48\xb8' + struct.pack('<Q', filetime) + b'\x48\x89\x01\xc3'
    functions = []
    for symbol in symbols:
        following = min(item.address for item in exports if item.address > symbol.address)
        if following - symbol.address < len(code):
            raise ValueError('Export interval is too short')
        original = image.get_data(symbol.address, len(code))
        if len(original) != len(code):
            raise ValueError('Incomplete original function bytes')
        functions.append({'name': symbol.name.decode('ascii'), 'rva': symbol.address,
                          'original_code': original.hex(), 'fixed_code': code.hex()})
    image.close()
    return {'system_dll': str(system_dll), 'system_dll_sha256': system_hash, 'functions': functions}


def capture(source, source_version, runtimes, unix_time):
    verify_platform(unix_time)
    data = reviewed_metadata(source, source_version)
    pins = {role: {'executable': str(path.with_suffix('.bin').resolve(strict=True)),
                   'executable_sha256': digest(path.with_suffix('.bin')),
                   'sal_sha256': digest(path.with_name('sal3.dll'))} for role, path in runtimes.items()}
    return {'version': 1, 'kind': KIND, 'unix_time': unix_time,
            'utc': datetime.fromtimestamp(unix_time, timezone.utc).isoformat(),
            'local_time': datetime.fromtimestamp(unix_time).isoformat(),
            'metadata_sha256': digest(METADATA), 'source_evidence': data['source_evidence'],
            'helper_sha256': digest(Path(__file__)), 'runtime_pins': pins, **system_functions(unix_time)}


def audit(control, source, source_version):
    verify_platform(control['unix_time'])
    metadata = reviewed_metadata(source, source_version)
    if (control.get('version') != 1 or control.get('kind') != KIND
            or digest(METADATA) != control.get('metadata_sha256')
            or digest(Path(__file__)) != control.get('helper_sha256')
            or metadata['source_evidence'] != control.get('source_evidence')
            or datetime.fromtimestamp(control['unix_time']).isoformat() != control.get('local_time')
            or any(control.get(key) != value for key, value in system_functions(control['unix_time']).items())):
        raise ValueError('Clock identity or evidence changed')
    for pin in control['runtime_pins'].values():
        executable = Path(pin['executable'])
        if digest(executable) != pin['executable_sha256'] or digest(executable.with_name('sal3.dll')) != pin['sal_sha256']:
            raise ValueError('Clock runtime identity changed')


def check_receipt(data, envelope, role, returncode):
    control = envelope['control']
    pin = control['runtime_pins'][role]
    if (type(returncode) is not int or data.get('returncode') != returncode
            or type(data.get('pid')) is not int or data['pid'] <= 0
            or data.get('control_sha256') != envelope['sha256']
            or data.get('role') != role or data.get('unix_time') != control['unix_time']
            or data.get('helper_sha256') != control['helper_sha256']
            or data.get('executable_sha256') != pin['executable_sha256']
            or data.get('sal_sha256') != pin['sal_sha256']
            or any(data.get(key) != control[key] for key in ('system_dll', 'system_dll_sha256', 'functions'))
            or any(data.get(key) is not True for key in ('clock_applied', 'patched_before_initial_breakpoint',
                   'executable_disk_unchanged', 'sal_disk_unchanged', 'system_dll_disk_unchanged'))
            or type(data.get('exceptions_forwarded')) is not int or data['exceptions_forwarded'] < 0):
        raise ValueError('Clock receipt missing, changed or incomplete')


def controlled_conversion(cmd, out, env, timeout, envelope, observations, runner):
    """One monotonic budget, only normal profile restart 81, never crash retry.

    The ordinary owner terminates this helper/tree on timeout or cancellation.
    Debugger exit kills its sole child. All receipts/files remain as evidence.
    """
    role = out.parent.name
    control = envelope['control']
    pin = control['runtime_pins'][role]
    if str(Path(cmd[0]).with_suffix('.bin').resolve()) != pin['executable']:
        raise ValueError('Clock converter does not match owned runtime')
    if digest(Path(envelope['path'])) != envelope['sha256']:
        raise ValueError('Clock control file changed')
    started = time.monotonic()
    for attempt in range(3):
        remaining = timeout - (time.monotonic() - started)
        if remaining <= 0:
            raise subprocess.TimeoutExpired(cmd, timeout)
        receipt = out.parent / f'clock_receipt_{attempt}.json'
        transformed = [sys.executable, str(Path(__file__).resolve()), '--control', envelope['path'],
                       '--control-sha256', envelope['sha256'], '--role', role,
                       '--receipt', str(receipt), '--', pin['executable'], *cmd[1:]]
        completed = runner(transformed, out, env, remaining)
        data = json.loads(receipt.read_text(encoding='utf-8'))
        check_receipt(data, envelope, role, completed.returncode)
        observations.append({'path': str(receipt), 'sha256': digest(receipt), 'data': data})
        if completed.returncode != 81:
            return completed
        if any(path.suffix.casefold() == '.pdf' for path in out.iterdir()):
            raise ValueError('Unexpected PDF during normal profile restart')
    raise ValueError('Normal profile restart limit exceeded')


def check_run(run, envelope, role):
    receipts = run['clock_receipts']
    if not 1 <= len(receipts) <= 3:
        raise ValueError('Missing or excessive clock receipts')
    for index, entry in enumerate(receipts):
        path = Path(entry['path'])
        if path.name != f'clock_receipt_{index}.json' or path.parent != Path(run['input_snapshot']).parent:
            raise ValueError('Clock receipt outside owned run')
        if digest(path) != entry['sha256'] or json.loads(path.read_text(encoding='utf-8')) != entry['data']:
            raise ValueError('Clock receipt changed after test')
        check_receipt(entry['data'], envelope, role, entry['data']['returncode'])
        expected = 81 if index < len(receipts) - 1 else (0 if run['status'] == 'SUCCESS' else run['returncode'])
        if entry['data']['returncode'] != expected:
            raise ValueError('Clock return code inconsistent with conversion')


def report_audit(report):
    """Validate retained receipts through trial, including normally refused inputs."""
    envelope = report['clock_control']
    control = envelope['control']
    if report.get('report_version') != 4 or report.get('audits', {}).get('clock_control_unchanged') is not True:
        raise ValueError('Missing formal clock audit')
    if digest(Path(envelope['path'])) != envelope['sha256'] or json.loads(Path(envelope['path']).read_text(encoding='utf-8')) != control:
        raise ValueError('Clock control changed after test')
    audit(control, Path(report['source_dir']), report['source_version'])
    for role, runtime in report['runtimes'].items():
        pin = control['runtime_pins'][role]
        if (pin['executable'] != str(Path(runtime['soffice']).with_suffix('.bin'))
                or pin['executable_sha256'] != runtime['sha256']['program/soffice.bin']
                or pin['sal_sha256'] != runtime['sha256']['program/sal3.dll']):
            raise ValueError('Report runtime does not match clock control')
    for record in report['results']:
        for role, run in record['runs'].items():
            check_run(run, envelope, role)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', required=True)
    parser.add_argument('--control-sha256', required=True)
    parser.add_argument('--role', choices=('baseline', 'candidate'), required=True)
    parser.add_argument('--receipt', required=True)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    control_path = Path(args.control).resolve(strict=True)
    if digest(control_path) != args.control_sha256:
        raise ValueError('Pinned clock control changed')
    control = json.loads(control_path.read_text(encoding='utf-8'))
    verify_platform(control['unix_time'])
    if control.get('helper_sha256') != digest(Path(__file__)):
        raise ValueError('Pinned clock helper changed')
    pin = control['runtime_pins'][args.role]
    command = args.command[1:] if args.command and args.command[0] == '--' else args.command
    if not command or '--headless' not in command or str(Path(command[0]).resolve(strict=True)) != pin['executable']:
        raise ValueError('Only the explicitly owned headless soffice.bin is supported')
    executable = Path(command[0]).resolve(strict=True)
    sal = executable.with_name('sal3.dll')
    if digest(sal) != pin['sal_sha256'] or digest(executable) != pin['executable_sha256']:
        raise ValueError('Pinned LibreOffice binary changed')
    receipt = Path(args.receipt).resolve()
    if receipt.exists() or not receipt.parent.is_dir():
        raise ValueError('Receipt requires a new filename in the owned QA output')
    if any(control.get(key) != value for key, value in system_functions(control['unix_time']).items()):
        raise ValueError('Pinned system DLL or exports changed')
    system_dll = Path(control['system_dll'])
    system_hash = control['system_dll_sha256']
    functions = control['functions']
    code = bytes.fromhex(functions[0]['fixed_code'])

    api = ctypes.WinDLL('kernel32', use_last_error=True)
    signatures = {
        'WaitForDebugEvent': ([ctypes.POINTER(DebugEvent), W.DWORD], W.BOOL),
        'ContinueDebugEvent': ([W.DWORD, W.DWORD, W.DWORD], W.BOOL),
        'DebugSetProcessKillOnExit': ([W.BOOL], W.BOOL),
        'GetFinalPathNameByHandleW': ([W.HANDLE, W.LPWSTR, W.DWORD, W.DWORD], W.DWORD),
        'CloseHandle': ([W.HANDLE], W.BOOL),
        'ReadProcessMemory': ([W.HANDLE, W.LPCVOID, W.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)], W.BOOL),
        'WriteProcessMemory': ([W.HANDLE, W.LPVOID, W.LPCVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)], W.BOOL),
        'VirtualProtectEx': ([W.HANDLE, W.LPVOID, ctypes.c_size_t, W.DWORD, ctypes.POINTER(W.DWORD)], W.BOOL),
        'FlushInstructionCache': ([W.HANDLE, W.LPCVOID, ctypes.c_size_t], W.BOOL),
    }
    for name, (argtypes, restype) in signatures.items():
        function = getattr(api, name)
        function.argtypes, function.restype = argtypes, restype
    def require(value):
        if not value:
            raise ctypes.WinError(ctypes.get_last_error())
        return value
    def path_for_handle(handle):
        buffer = ctypes.create_unicode_buffer(32768)
        size = require(api.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0))
        if size >= len(buffer):
            raise ValueError('Module path exceeds buffer')
        value = buffer.value
        if value.startswith('\\\\?\\'):
            value = value[4:]
        return Path(value).resolve(strict=True)
    def read_at(process, address):
        buffer = ctypes.create_string_buffer(len(code))
        count = ctypes.c_size_t()
        require(api.ReadProcessMemory(process, address, buffer, len(code), ctypes.byref(count)))
        if count.value != len(code):
            raise ValueError('Short code read')
        return buffer.raw
    observations = {'unix_time': control['unix_time'], 'sal_sha256': pin['sal_sha256'],
                    'role': args.role, 'control_sha256': args.control_sha256,
                    'helper_sha256': control['helper_sha256'], 'executable_sha256': pin['executable_sha256'],
                    'sal_path': str(sal), 'system_dll': str(system_dll), 'system_dll_sha256': system_hash,
                    'functions': functions, 'patched_before_initial_breakpoint': False,
                    'exceptions_forwarded': 0}
    # Debug only this new child; the outer QA runner owns its tree/stdout pipe.
    child = subprocess.Popen(command, stdin=subprocess.DEVNULL, creationflags=0x2 | 0x08000000)
    observations['pid'] = child.pid
    require(api.DebugSetProcessKillOnExit(True))
    patched, initial_breakpoint = False, False
    while True:
        event = DebugEvent()
        if not api.WaitForDebugEvent(ctypes.byref(event), 1000):
            error = ctypes.get_last_error()
            if error == 121:  # ERROR_SEM_TIMEOUT; outer owned-process timeout remains authoritative.
                continue
            raise ctypes.WinError(error)
        if event.pid != child.pid:
            raise ValueError('Unexpected debug process')
        continuation = 0x00010002  # DBG_CONTINUE
        if event.code == 3 and event.data.create.file:
            require(api.CloseHandle(event.data.create.file))
        elif event.code == 6 and event.data.dll.file:
            try:
                loaded = path_for_handle(event.data.dll.file)
                if loaded == system_dll:
                    if patched or initial_breakpoint or digest(loaded) != system_hash:
                        raise ValueError('Unexpected system clock DLL load or identity')
                    for function in functions:
                        address = event.data.dll.base + function['rva']
                        if read_at(child._handle, address) != bytes.fromhex(function['original_code']):
                            raise ValueError('Loaded system clock function differs from pinned PE bytes')
                        protection = W.DWORD()
                        require(api.VirtualProtectEx(child._handle, address, len(code), 0x40, ctypes.byref(protection)))
                        try:
                            count = ctypes.c_size_t()
                            require(api.WriteProcessMemory(child._handle, address, code, len(code), ctypes.byref(count)))
                            if count.value != len(code) or read_at(child._handle, address) != code:
                                raise ValueError('Clock patch verification failed')
                        finally:
                            discarded = W.DWORD()
                            require(api.VirtualProtectEx(child._handle, address, len(code), protection.value, ctypes.byref(discarded)))
                        require(api.FlushInstructionCache(child._handle, address, len(code)))
                    observations['patched_before_initial_breakpoint'] = True
                    patched = True
            finally:
                require(api.CloseHandle(event.data.dll.file))
        elif event.code == 1:
            exception = event.data.exception
            if not initial_breakpoint and exception.record.code == 0x80000003 and exception.first:
                if not patched:
                    raise ValueError('System clock not patched before initial process breakpoint')
                initial_breakpoint = True
            else:
                continuation = 0x80010001  # DBG_EXCEPTION_NOT_HANDLED: preserve real exception/crash behavior.
                observations['exceptions_forwarded'] += 1
        elif event.code == 5:
            observations.update(returncode=int(event.data.exit), clock_applied=patched and initial_breakpoint,
                                executable_disk_unchanged=digest(executable) == pin['executable_sha256'],
                                sal_disk_unchanged=digest(sal) == pin['sal_sha256'],
                                system_dll_disk_unchanged=digest(system_dll) == system_hash)
            require(api.ContinueDebugEvent(event.pid, event.tid, continuation))
            child.wait()
            with receipt.open('x', encoding='utf-8') as stream:
                json.dump(observations, stream, indent=2)
            check_receipt(observations, {'control': control, 'sha256': args.control_sha256}, args.role, observations['returncode'])
            return observations['returncode']
        require(api.ContinueDebugEvent(event.pid, event.tid, continuation))

def windows_exit_argument(returncode):
    # CPython sys.exit passes a signed C long on Windows. Preserve high-bit
    # crash NTSTATUS DWORDs instead of overflowing them into helper exit -1.
    if type(returncode) is not int or not 0 <= returncode <= 0xffffffff:
        raise ValueError('Invalid Windows process exit DWORD')
    return returncode if returncode < 0x80000000 else returncode - 0x100000000


if __name__ == '__main__':
    try:
        sys.exit(windows_exit_argument(main()))
    except Exception as error:
        print(f'QA clock control failed: {type(error).__name__}: {error}', file=sys.stderr)
        sys.exit(2)
