#!/usr/bin/env python3
"""
RenderDoc Python Module Test Suite

Tests the built RenderDoc Python modules to ensure they work correctly.
"""

import sys
from pathlib import Path


def test_module_import(module_path):
    """Test if a module can be imported."""
    print(f"\n{'='*60}")
    print(f"Testing: {module_path.name}")
    print(f"{'='*60}")

    module_dir = str(module_path)
    if module_dir not in sys.path:
        sys.path.insert(0, module_dir)

    for mod in list(sys.modules.keys()):
        if mod.startswith('renderdoc') or mod.startswith('qrenderdoc'):
            del sys.modules[mod]

    results = {}

    print("\n[*] Testing renderdoc module...")
    try:
        import renderdoc

        results["renderdoc"] = {"status": "PASS", "error": None}
        print("   [+] Import successful")

        attrs_to_check = [
            "CaptureFile",
            "ReplayStatus",
            "StructuredData",
            "ShaderReflection",
            "APIProperties",
        ]

        for attr in attrs_to_check:
            if hasattr(renderdoc, attr):
                print(f"      [+] {attr}")
            else:
                print(f"      [-] {attr} (missing)")

        if hasattr(renderdoc, "VERSION_STRING"):
            print(f"      [*] Version: {renderdoc.VERSION_STRING}")

    except Exception as e:
        results["renderdoc"] = {"status": "FAIL", "error": str(e)}
        print(f"   [-] Import failed: {e}")

    print("\n[*] Testing qrenderdoc module...")
    try:
        import qrenderdoc

        results["qrenderdoc"] = {"status": "PASS", "error": None}
        print("   [+] Import successful")

        attrs_to_check = [
            "CaptureContext",
            "ReplayController",
        ]

        for attr in attrs_to_check:
            if hasattr(qrenderdoc, attr):
                print(f"      [+] {attr}")
            else:
                print(f"      [-] {attr} (missing)")

    except Exception as e:
        results["qrenderdoc"] = {"status": "FAIL", "error": str(e)}
        print(f"   [-] Import failed: {e}")

    return results


def check_dll_dependencies(module_path, required_dlls):
    """Check if required DLLs are present."""
    print(f"\n[*] Checking DLL dependencies...")

    missing_dlls = []

    for dll in required_dlls:
        dll_path = module_path / dll
        if dll_path.exists():
            size_mb = dll_path.stat().st_size / (1024 * 1024)
            print(f"   [+] {dll} ({size_mb:.1f} MB)")
        else:
            print(f"   [-] {dll} (MISSING)")
            missing_dlls.append(dll)

    return missing_dlls


def check_module_files(module_path):
    """Check if .pyd files exist and their sizes."""
    print(f"\n[*] Checking module files...")

    pyd_files = {
        "renderdoc.pyd": "Core RenderDoc module",
        "qrenderdoc.pyd": "Qt UI module",
    }

    for pyd, description in pyd_files.items():
        pyd_path = module_path / pyd
        if pyd_path.exists():
            size_mb = pyd_path.stat().st_size / (1024 * 1024)
            print(f"   [+] {pyd}: {size_mb:.1f} MB - {description}")
        else:
            print(f"   [-] {pyd}: MISSING - {description}")


def _load_required_dlls():
    """Load required DLL list from config, with fallback."""
    try:
        import json

        config_path = Path(__file__).parent.parent / "assets" / "config.json"
        if config_path.exists():
            with open(config_path, encoding="utf-8") as f:
                cfg = json.load(f)
            return cfg["dependencies"]["required_dlls"]
    except Exception:
        pass
    return ["renderdoc.dll", "d3dcompiler_47.dll"]


def run_all_tests(module_path=None):
    """Run complete test suite."""
    print("\n" + "=" * 60)
    print("RenderDoc Python Module Test Suite")
    print("=" * 60)

    if module_path is None:
        module_path = Path.cwd() / "x64" / "Development" / "pymodules"
    else:
        module_path = Path(module_path)

    print(f"\n[*] Module path: {module_path}")

    if not module_path.exists():
        print(f"\n[!] ERROR: Module path does not exist: {module_path}")
        return False

    check_module_files(module_path)

    required_dlls = _load_required_dlls()
    missing_dlls = check_dll_dependencies(module_path, required_dlls)

    results = test_module_import(module_path)

    print(f"\n{'='*60}")
    print("Test Summary")
    print(f"{'='*60}")

    all_passed = True
    for module, result in results.items():
        status = result["status"]
        symbol = "[+]" if status == "PASS" else "[-]"
        print(f"   {symbol} {module}: {status}")
        if result["error"]:
            print(f"      Error: {result['error']}")
        if status == "FAIL":
            all_passed = False

    if missing_dlls:
        print(f"\n[!] Warning: Missing DLLs: {', '.join(missing_dlls)}")
        all_passed = False

    print()
    if all_passed:
        print("[+] All tests PASSED!")
        return True
    else:
        print("[!] Some tests FAILED!")
        return False


def main():
    import argparse

    parser = argparse.ArgumentParser(
        description="Test RenderDoc Python modules"
    )
    parser.add_argument(
        "--module-path",
        help="Path to pymodules directory (default: x64/Development/pymodules)",
    )

    args = parser.parse_args()

    success = run_all_tests(args.module_path)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
