#!/usr/bin/env python3
"""Tests for overlay-desktop/usr/bin/cloke without any real package manager."""
import importlib.machinery
import importlib.util
import hashlib
import io
import json
import os
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

path = os.path.join(os.path.dirname(__file__), "..", "overlay-desktop", "usr", "bin", "cloke")
loader = importlib.machinery.SourceFileLoader("cloke", path)
cloke = importlib.util.module_from_spec(importlib.util.spec_from_loader("cloke", loader))
loader.exec_module(cloke)


class Cloke(unittest.TestCase):
    def setUp(self):
        self.p = [mock.patch.object(cloke, "HOST", "macos"), mock.patch.object(cloke, "have", lambda b: b == "brew"),
                  mock.patch.object(cloke, "appimage_feed", lambda: []),
                  mock.patch.object(cloke, "layer_available", lambda l: False),
                  mock.patch.object(cloke, "OS_ORDER", ["macos", "linux", "windows"])]
        for x in self.p:
            x.start()
        self.addCleanup(lambda: [x.stop() for x in self.p])

    def test_steam_lists_all_oses_and_recommends_linux_dnf(self):
        with mock.patch.object(cloke, "run", lambda *a, **k: "steam\n" if "--cask" in a[0] else ""):
            picks = cloke.find("steam")
        self.assertEqual([p.label() for p in picks], ["Steam-macOS (brew, cask)", "Steam-Linux (dnf)", "Steam-Linux (flatpak)",
                                                      "Steam-Windows (winget)", "Steam-Windows (choco)"])
        self.assertEqual(cloke.recommended(picks), 1)

    def test_formula_beats_everything(self):
        with mock.patch.object(cloke, "run", lambda *a, **k: "git\n" if "--formula" in a[0] else ""):
            picks = cloke.find("git")
        self.assertEqual(picks[cloke.recommended(picks)].label(), "Git-macOS (brew)")

    def test_unknown_program_only_live_hits(self):
        with mock.patch.object(cloke, "run", lambda *a, **k: "figlet\nfiglet-fonts\n" if "--formula" in a[0] else ""):
            picks = cloke.find("figlet")
        self.assertEqual([p.id for p in picks], ["figlet"])

    def test_winget_table_parser(self):
        out = "Name  Id          Version Source\n-----------------------------\nSteam Valve.Steam 1.0   winget\n"
        with mock.patch.object(cloke, "run", lambda *a, **k: out):
            pk = cloke.search_winget("steam")
        self.assertEqual((pk[0].id, pk[0].version), ("Valve.Steam", "1.0"))

    def test_dnf_and_flatpak_parsers(self):
        with mock.patch.object(cloke, "run", lambda *a, **k: "steam.x86_64 : Valve client\nsteam-devices.noarch : udev\n"):
            self.assertEqual(cloke.search_dnf("steam")[0].id, "steam")
        with mock.patch.object(cloke, "run", lambda *a, **k: "com.valvesoftware.Steam\tSteam\tclient\n"):
            self.assertEqual(cloke.search_flatpak("steam")[0].id, "com.valvesoftware.Steam")

    def test_missing_layer_is_reported_not_run(self):
        pk = cloke.Pkg("dnf", "steam", "steam")
        with mock.patch.object(cloke.subprocess, "call") as call:
            self.assertEqual(cloke.do_install(pk)[0], 2)
            call.assert_not_called()


def make_tgz(path, files):
    with tarfile.open(path, "w:gz") as t:
        for name, data in files.items():
            ti = tarfile.TarInfo(name)
            ti.size = len(data)
            t.addfile(ti, io.BytesIO(data))


class Repo(unittest.TestCase):
    """App repository, cache and scopes against a local index (no network, no real HOME)."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(lambda: __import__("shutil").rmtree(self.tmp, ignore_errors=True))
        self.tgz = os.path.join(self.tmp, "dillo.tgz")
        make_tgz(self.tgz, {"Applications/Dillo.app/Contents/Info.plist": b"x"})
        sha = hashlib.sha256(open(self.tgz, "rb").read()).hexdigest()
        self.index = os.path.join(self.tmp, "index.json")
        entry = lambda i, n, cat, path, sha_: {"id": i, "name": n, "name_de": n, "path": path, "category": cat, "version": "2.8",
                                               "package": {"url": "file://" + self.tgz, "sha256": sha_, "size": 1}, "recommends": []}
        json.dump({"apps": [entry("org.dillo.dillo", "Dillo", "addon", "/Applications/Dillo.app", sha),
                            entry("com.clokeos.rechner", "Calculator", "system", "/System/Applications/Calculator.app", sha),
                            entry("org.bad.bad", "Bad", "addon", "/Applications/Bad.app", "0" * 64)],
                   "packages": []}, open(self.index, "w"))
        self.p = [mock.patch.object(cloke, "HOST", "macos"), mock.patch.object(cloke, "APPS_INDEX", self.index),
                  mock.patch.object(cloke, "CACHE", os.path.join(self.tmp, "cache")),
                  mock.patch.object(cloke, "USER_APPS", os.path.join(self.tmp, "Applications")),
                  mock.patch.object(cloke, "STATE", os.path.join(self.tmp, "state.json")),
                  mock.patch.object(cloke, "SYSTEM_STATE", os.path.join(self.tmp, "sys.json")),
                  mock.patch.object(cloke, "SCOPE", "user"), mock.patch.object(cloke, "OFFLINE", False),
                  mock.patch.object(cloke, "have", lambda b: b == "apps")]
        for x in self.p:
            x.start()
        self.addCleanup(lambda: [x.stop() for x in self.p])

    def test_search_finds_app_by_name_and_id(self):
        self.assertEqual([p.id for p in cloke.search_apps("dillo")], ["org.dillo.dillo"])
        self.assertEqual([p.id for p in cloke.search_apps("rechner")], ["com.clokeos.rechner"])

    def test_user_install_goes_to_home_applications_and_is_cached(self):
        pk = cloke.search_apps("dillo")[0]
        v, path = cloke.apps_install(pk)
        self.assertEqual((v, path), ("2.8", os.path.join(self.tmp, "Applications", "Dillo.app")))
        self.assertTrue(os.path.exists(os.path.join(path, "Contents", "Info.plist")))
        # zweite Installation ohne Netz: das Paket kommt aus dem Cache
        os.remove(self.tgz)
        with mock.patch.object(cloke, "OFFLINE", True):
            v, path = cloke.apps_install(pk)
        self.assertTrue(os.path.isdir(path))

    def test_system_app_refused_for_user_scope(self):
        pk = cloke.search_apps("rechner")[0]
        with self.assertRaises(RuntimeError) as cm:
            cloke.apps_install(pk)
        self.assertIn("--system", str(cm.exception))

    def test_bad_checksum_installs_nothing(self):
        pk = cloke.search_apps("bad")[0]
        with self.assertRaises(RuntimeError) as cm:
            cloke.apps_install(pk)
        self.assertIn("checksum", str(cm.exception))
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "Applications", "Bad.app")))

    def test_offline_search_uses_cached_index(self):
        cloke.search_apps("dillo")
        os.remove(self.index)
        with mock.patch.object(cloke, "OFFLINE", True):
            self.assertEqual(len(cloke.search_apps("dillo")), 1)

    def test_missing_index_without_cache_is_an_error_not_a_crash(self):
        os.remove(self.index)
        self.assertEqual(cloke.search_apps("dillo"), [])

    def test_state_record_forget_and_remove(self):
        pk = cloke.search_apps("dillo")[0]
        v, path = cloke.apps_install(pk)
        cloke.record({"name": "dillo", "backend": "apps", "id": pk.id, "os": "macos", "version": v, "scope": "user", "path": path})
        self.assertEqual(len(cloke.all_state()), 1)
        ns = type("A", (), {"name": "dillo"})
        self.assertEqual(cloke.cmd_remove(ns), 0)
        self.assertFalse(os.path.exists(path))
        self.assertEqual(cloke.all_state(), [])

    def test_remove_refuses_foreign_paths(self):
        self.assertEqual(cloke.apps_remove({"path": "/etc"}), 1)

    def test_apk_parser(self):
        with mock.patch.object(cloke, "layer_command", lambda l: ["tux"]), \
             mock.patch.object(cloke, "run", lambda *a, **k: "figlet-2.2.5-r3 - Program for letters\nfiglet-doc-2.2.5-r3 - docs\n"):
            r = cloke.search_apk("figlet")
        self.assertEqual((r[0].id, r[0].version, r[0].score), ("figlet", "2.2.5-r3", 3))

    def test_elevate_without_sudo_says_so(self):
        with mock.patch.object(cloke.os, "geteuid", lambda: 501), mock.patch.object(cloke.shutil, "which", lambda c: None):
            with self.assertRaises(cloke.NeedRoot) as cm:
                cloke.elevate(["apk"], "Installing Linux packages")
        self.assertIn("no sudo", str(cm.exception))

    def test_version_order(self):
        self.assertGreater(cloke.vkey("2.10"), cloke.vkey("2.9"))


if __name__ == "__main__":
    unittest.main()
