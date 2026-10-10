"""IDE root-owner binding contracts with fake User32, processes and threads."""
import json
from types import SimpleNamespace
import subprocess
import unittest
from unittest.mock import patch

from collection_live_session import WindowsBackend


class FakeUser32:
    def __init__(self):
        self.calls=[]
        self.foreground=11
        self.root=90
        self.pids={90:9001,91:9001,80:8001}
        self.clients={90:(0,0,1100,700),91:(0,0,500,350)}
        self.pid_sequences={}
        self.client_sequences={}

    def GetForegroundWindow(self):
        self.calls.append(('foreground',self.foreground));return self.foreground

    def GetAncestor(self,hwnd,mode):
        self.calls.append(('ancestor',hwnd,mode));return self.root

    def GetWindowThreadProcessId(self,hwnd,pid):
        self.calls.append(('pid',hwnd))
        sequence=self.pid_sequences.get(hwnd)
        value=sequence.pop(0) if sequence else self.pids.get(hwnd,0)
        pid.value=value
        return 1 if value else 0

    def GetClientRect(self,hwnd,rect):
        self.calls.append(('client',hwnd))
        sequence=self.client_sequences.get(hwnd)
        value=sequence.pop(0) if sequence else self.clients.get(hwnd)
        if value is None:return False
        rect.left,rect.top,rect.right,rect.bottom=value
        return True

    def SetThreadDpiAwarenessContext(self,value):
        self.calls.append(('dpi',value));return 1

    def GetCursorPos(self,point):
        self.calls.append(('cursor',));point.x=100;point.y=100;return True


class WindowBindingTests(unittest.TestCase):
    def setUp(self):
        self.backend=object.__new__(WindowsBackend)
        self.backend.u=FakeUser32()
        self.backend.c=SimpleNamespace(byref=lambda obj:obj,c_void_p=lambda value:value)
        self.backend.w=SimpleNamespace(DWORD=lambda:SimpleNamespace(value=0),
            RECT=lambda:SimpleNamespace(left=0,top=0,right=0,bottom=0),
            POINT=lambda:SimpleNamespace(x=0,y=0))
        self.backend._metadata={}
        self.backend.identities={}
        self.backend.halt=SimpleNamespace(is_set=lambda:True)
        self.activations=[];self.threads=[]
        def thread(**options):
            state=dict(options,started=False);self.threads.append(state)
            return SimpleNamespace(start=lambda:state.update(started=True))
        self.backend.threading=SimpleNamespace(Thread=thread)
        self.backend.activate=lambda hwnd,pid:self.activations.append((hwnd,pid))
        self.enumerated=dict(target_hwnd='80',target_pid='8001',return_hwnd='91',return_pid='9001')

    def enter(self, *, enum_exit=0,after_enum=None):
        def process(*args,**kwargs):
            self.backend.u.calls.append(('enumerate',))
            if after_enum:after_enum()
            return subprocess.CompletedProcess(args,enum_exit,json.dumps(self.enumerated).encode(),b'')
        with patch('collection_live_session.subprocess.run',side_effect=process):
            self.backend.enter()

    def assert_no_foreground_change(self):
        self.assertEqual(self.activations,[])
        self.assertEqual(self.threads,[])
        self.assertFalse(any(c[0]=='dpi' for c in self.backend.u.calls))

    def test_entry_root_owner_replaces_disappeared_dotnet_popup(self):
        self.backend.u.pids.pop(91);self.backend.u.clients.pop(91)
        self.enter()
        self.assertEqual(self.backend.identities['return_hwnd'],90)
        self.assertEqual(self.backend._metadata['return_window_binding'],'entry_foreground_root_owner_same_pid')
        self.assertIn(('ancestor',11,3),self.backend.u.calls)
        self.assertEqual(self.activations,[(80,8001)])
        self.assertTrue(self.threads[0]['started'])

    def test_process_enumeration_uses_absolute_powershell_without_host_path_search(self):
        reply=subprocess.CompletedProcess([],0,json.dumps(self.enumerated).encode(),b'')
        with patch.dict('collection_live_session.os.environ',{'SystemRoot':r'C:\Windows','PATH':''}), \
                patch('collection_live_session.subprocess.run',return_value=reply) as launch:
            self.backend.enter()
        command=launch.call_args.args[0]
        self.assertEqual(command[0],r'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe')
        self.assertEqual(command[1:4],['-NoProfile','-NonInteractive','-Command'])

    def test_root_owner_uses_foreground_snapshot_from_before_process_enumeration(self):
        self.enter(after_enum=lambda:setattr(self.backend.u,'foreground',777))
        self.assertEqual(self.backend.u.calls[0],('foreground',11))
        self.assertIn(('ancestor',11,3),self.backend.u.calls)
        self.assertNotIn(('ancestor',777,3),self.backend.u.calls)
        self.assertEqual(self.backend.identities['return_hwnd'],90)

    def test_foreign_pid_root_uses_independently_validated_enumerated_ide(self):
        self.backend.u.pids[90]=9999
        self.enter()
        self.assertEqual(self.backend.identities['return_hwnd'],91)
        self.assertIn(('pid',91),self.backend.u.calls)
        self.assertIn(('client',91),self.backend.u.calls)
        self.assertEqual(self.activations,[(80,8001)])

    def test_missing_zero_or_failed_client_root_falls_back_to_valid_ide(self):
        for invalid in (None,(0,0,0,700),(0,0,1100,0)):
            with self.subTest(invalid=invalid):
                self.setUp();self.backend.u.clients[90]=invalid;self.enter()
                self.assertEqual(self.backend.identities['return_hwnd'],91)
                self.assertIn(('pid',91),self.backend.u.calls)
                self.assertIn(('client',91),self.backend.u.calls)

    def test_zero_root_falls_back_without_assuming_a_pid(self):
        self.backend.u.root=0;self.enter()
        self.assertEqual(self.backend.identities['return_hwnd'],91)
        self.assertIn(('pid',91),self.backend.u.calls)

    def test_stale_enumerated_popup_fails_before_game_activation(self):
        self.backend.u.pids[90]=9999
        self.backend.u.pids.pop(91);self.backend.u.clients.pop(91)
        with self.assertRaisesRegex(RuntimeError,'RETURN_WINDOW'):
            self.enter()
        self.assert_no_foreground_change()

    def test_wrong_pid_enumerated_handle_fails_before_game_activation(self):
        self.backend.u.root=0;self.backend.u.pids[91]=9999
        with self.assertRaisesRegex(RuntimeError,'RETURN_WINDOW'):
            self.enter()
        self.assert_no_foreground_change()

    def test_final_enumerated_empty_client_fails_before_game_activation(self):
        self.backend.u.root=0;self.backend.u.clients[91]=(0,0,0,0)
        with self.assertRaisesRegex(RuntimeError,'RETURN_WINDOW'):
            self.enter()
        self.assert_no_foreground_change()

    def test_selected_root_disappearing_before_final_validation_fails_before_activation(self):
        self.backend.u.client_sequences[90]=[(0,0,1100,700),None]
        with self.assertRaisesRegex(RuntimeError,'RETURN_WINDOW'):
            self.enter()
        self.assert_no_foreground_change()

    def test_selected_root_pid_changing_before_final_validation_fails_before_activation(self):
        self.backend.u.pid_sequences[90]=[9001,9999]
        with self.assertRaisesRegex(RuntimeError,'RETURN_WINDOW'):
            self.enter()
        self.assert_no_foreground_change()

    def test_failed_process_enumeration_performs_no_focus_or_watcher_work(self):
        with self.assertRaisesRegex(RuntimeError,'COLLECTION_WINDOW_IDENTITY'):
            self.enter(enum_exit=1)
        self.assert_no_foreground_change()
        self.assertFalse(any(c[0]=='ancestor' for c in self.backend.u.calls))


if __name__=='__main__':unittest.main()
