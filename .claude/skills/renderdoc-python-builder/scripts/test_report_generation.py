#!/usr/bin/env python3
"""
Test script to verify report generation functionality.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from skill import RenderDocPyBuilder


def test_report_generation():
    """Test the report generation with simulated build."""

    print("[*] Testing Report Generation")
    print("=" * 60)

    builder = RenderDocPyBuilder(config="Development", platform_arch="x64")

    # Simulate build results
    builder.build_results = {
        "modules": {
            "pyrenderdoc_module": {
                "project": str(builder.pyrenderdoc_project),
                "success": True,
                "duration": 123.5,
                "stdout": "Build output...",
                "stderr": "",
            },
            "qrenderdoc_module": {
                "project": str(builder.qrenderdoc_project),
                "success": True,
                "duration": 45.2,
                "stdout": "Build output...",
                "stderr": "",
            },
        },
        "dependencies": {
            "renderdoc.dll": {
                "path": str(builder.output_dir / "renderdoc.dll"),
                "size_mb": 72.5,
                "copied": True,
            },
            "d3dcompiler_47.dll": {
                "path": str(builder.output_dir / "d3dcompiler_47.dll"),
                "size_mb": 4.5,
                "copied": False,
            },
        },
        "tests": {
            "renderdoc": {"status": "Pass", "error": None},
            "qrenderdoc": {"status": "Pass", "error": None},
        },
    }

    print("\nGenerating report...")
    report_path = builder.generate_report()

    print(f"\n[+] Report generated successfully!")
    print(f"    Location: {report_path}")
    print(f"    Size: {report_path.stat().st_size / 1024:.1f} KB")

    print("\n" + "=" * 60)
    print("Report Preview (first 30 lines):")
    print("=" * 60)
    report_content = report_path.read_text(encoding="utf-8")
    lines = report_content.split("\n")
    for i, line in enumerate(lines[:30], 1):
        print(f"{i:3d}: {line}")

    print("\n...")
    print(f"\n[*] Total lines: {len(lines)}")


if __name__ == "__main__":
    try:
        test_report_generation()
    except Exception as e:
        print(f"\n[-] Error: {e}")
        import traceback

        traceback.print_exc()
        sys.exit(1)
