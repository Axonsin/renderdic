#!/usr/bin/env python3
"""
RenderDoc Python Module Builder

Automates building RenderDoc Python modules for a specific Python version.
Reads all configurable paths and settings from assets/config.json.

Usage:
    python skill.py [--python-version VERSION] [--config CONFIG] [--platform PLATFORM]

Requirements:
    - Visual Studio 2019/2022/2026 with MSBuild
    - Python 3.x installed
    - RenderDoc source code
"""

import argparse
import json
import os
import subprocess
import sys
import shutil
import re
import platform
import datetime
from pathlib import Path
import xml.etree.ElementTree as ET


SKILL_DIR = Path(__file__).parent.parent  # scripts/ -> renderdoc-python-builder/
CONFIG_PATH = SKILL_DIR / "assets" / "config.json"


def load_config():
    """Load configuration from assets/config.json."""
    if not CONFIG_PATH.exists():
        raise FileNotFoundError(
            f"Configuration file not found: {CONFIG_PATH}. "
            f"Ensure assets/config.json exists."
        )
    with open(CONFIG_PATH, encoding="utf-8") as f:
        return json.load(f)


def _validate_project_root(config, project_root):
    """Validate a RenderDoc project root using the configured marker file."""
    marker = config["project"]["root_marker"]
    root = Path(project_root).resolve()
    if not (root / marker).exists():
        raise FileNotFoundError(
            f"Invalid RenderDoc project root: {root}. "
            f"Expected marker file: {marker}"
        )
    return root


def find_project_root(config, start_dir=None):
    """Find RenderDoc project root by searching upward for a marker file.

    This replaces the fragile N-level .parent chain with a marker-based search
    that works regardless of how deeply the skill directory is nested.
    """
    marker = config["project"]["root_marker"]
    current = Path(start_dir or SKILL_DIR).resolve()
    for _ in range(20):  # Safety limit
        candidate = current / marker
        if candidate.exists():
            return current
        parent = current.parent
        if parent == current:
            break
        current = parent
    raise FileNotFoundError(
        f"Cannot find RenderDoc project root. "
        f"Searched upward from {SKILL_DIR} looking for marker: {marker}"
    )


class RenderDocPyBuilder:
    """Build RenderDoc Python modules for a specific Python version."""

    def __init__(
        self,
        python_version=None,
        config="Development",
        platform_arch="x64",
        project_root=None,
        strict_stubs=False,
    ):
        # Load configuration
        self.cfg = load_config()

        # Resolve project root dynamically or use an explicit CI checkout path.
        if project_root:
            self.project_root = _validate_project_root(self.cfg, project_root)
        else:
            self.project_root = find_project_root(self.cfg)

        self.config = config
        self.platform = platform_arch
        self.strict_stubs = strict_stubs
        self.build_start_time = datetime.datetime.now()

        # Tool paths from config
        tools = self.cfg["tools"]
        self.vswhere_path = Path(tools["vswhere_path"])
        self.windows_kits_root = Path(tools["windows_kits_root"])

        # MSBuild information (must come before toolset auto-detect)
        self.msbuild_path = self._find_msbuild()
        self.msbuild_version = self._get_msbuild_version()

        # Auto-detect platform toolset if set to "auto"
        raw_toolset = tools.get("platform_toolset", "auto")
        if raw_toolset == "auto":
            self.platform_toolset = self._detect_platform_toolset()
        else:
            self.platform_toolset = raw_toolset

        # Detect Python environment
        self.python_exe = Path(sys.executable)
        self.python_version = python_version or self._detect_python_version()
        self.python_full_version = self._get_python_full_version()
        self.python_root = self.python_exe.parent
        self.python_include = self.python_root / "include"
        self.python_libs = self.python_root / "libs"

        # Find Python import library
        self.python_import_lib = self._find_python_lib()

        # Visual Studio information
        self.vs_info = self._get_visual_studio_info()

        # System information
        self.system_info = self._get_system_info()

        # Project paths from config
        proj = self.cfg["project"]
        self.pyrenderdoc_project = self.project_root / proj["vcxproj_paths"]["pyrenderdoc"]
        self.qrenderdoc_project = self.project_root / proj["vcxproj_paths"]["qrenderdoc"]
        self.version_header = self.project_root / proj["version_header"]
        self.stubs_script = self.project_root / proj["stubs_script"]

        # RenderDoc version
        self.renderdoc_version = self._get_renderdoc_version()

        # Git commit hash
        self.git_commit_hash = self._get_git_commit_hash()

        # Windows SDK version
        self.windows_sdk_version = self._get_windows_sdk_version()

        # Dependency names from config
        self.required_dlls = self.cfg["dependencies"]["required_dlls"]
        self.file_descriptions = self.cfg["dependencies"]["file_descriptions"]

        # Output paths
        self.output_dir = self.project_root / self.platform / self.config / "pymodules"
        self.renderdoc_dll = self.project_root / self.platform / self.config / "renderdoc.dll"

        # Build results storage
        self.build_results = {
            "modules": {},
            "dependencies": {},
            "tests": {},
        }

        print(f"[*] RenderDoc Python Module Builder")
        print(f"    Project root: {self.project_root}")
        print(f"    Python: {self.python_exe} ({self.python_version})")
        print(f"    Include: {self.python_include}")
        print(f"    Libs: {self.python_libs}")
        print(f"    Import lib: {self.python_import_lib}")
        print(f"    MSBuild: {self.msbuild_path}")
        print(f"    Platform Toolset: {self.platform_toolset}")
        print(f"    Windows SDK: {self.windows_sdk_version}")
        print(f"    Configuration: {self.config} | {self.platform}")
        print(f"    Strict stubs: {self.strict_stubs}")
        print(f"    Git Commit: {self.git_commit_hash[:12]}")

    def _detect_python_version(self):
        """Detect Python version using sys.version_info (works for any major version)."""
        vi = sys.version_info
        return f"{vi.major}.{vi.minor}"

    def _detect_platform_toolset(self):
        """Auto-detect the platform toolset from the VS installation."""
        try:
            msbuild_dir = self.msbuild_path.parent.parent.parent
            props_dir = msbuild_dir / "Microsoft" / "VC"
            if not props_dir.exists():
                raise FileNotFoundError(f"VC props dir not found: {props_dir}")

            def _version_key(d):
                m = re.match(r'v(\d+)', d.name)
                return int(m.group(1)) if m else 0

            vc_dirs = sorted(
                [d for d in props_dir.iterdir() if d.is_dir()],
                key=_version_key,
                reverse=True,
            )
            for vc_dir in vc_dirs:
                platform_props = vc_dir / "Platforms" / "x64" / "Platform.Default.props"
                if platform_props.exists():
                    content = platform_props.read_text(encoding="utf-8")
                    match = re.search(r'<DefaultPlatformToolset[^>]*>(\w+)</DefaultPlatformToolset>', content)
                    if match:
                        return match.group(1)
        except Exception as e:
            print(f"   [!]  Warning: Toolset auto-detection failed ({e})")
        print("   [!]  Warning: Could not auto-detect platform toolset, falling back to v143")
        return "v143"

    def _get_python_full_version(self):
        """Get full Python version string."""
        result = subprocess.run(
            [str(self.python_exe), "-c", "import sys; print(sys.version)"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            check=True,
        )
        return result.stdout.strip()

    def _get_msbuild_version(self):
        """Get MSBuild version."""
        try:
            result = subprocess.run(
                [str(self.msbuild_path), "-version", "-nologo"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=True,
            )
            match = re.search(r'(\d+\.\d+\.\d+\.\d+)', result.stdout)
            if match:
                return match.group(1)
            return "Unknown"
        except Exception:
            return "Unknown"

    def _get_visual_studio_info(self):
        """Get Visual Studio installation information via vswhere."""
        try:
            result = subprocess.run(
                [
                    str(self.vswhere_path),
                    "-latest",
                    "-property", "installationPath",
                    "-property", "displayName",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=True,
            )
            lines = result.stdout.strip().split('\n')
            return {
                "installation_path": lines[0] if len(lines) > 0 else "Unknown",
                "display_name": lines[1] if len(lines) > 1 else "Unknown",
            }
        except Exception:
            return {"installation_path": "Unknown", "display_name": "Unknown"}

    def _get_system_info(self):
        """Get detailed system information."""
        return {
            "os": platform.system(),
            "os_version": platform.version(),
            "os_release": platform.release(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "python_implementation": platform.python_implementation(),
        }

    def _get_renderdoc_version(self):
        """Get RenderDoc version from version.h header file."""
        try:
            version_file = self.version_header
            if version_file.exists():
                content = version_file.read_text()
                major_match = re.search(r'RENDERDOC_VERSION_MAJOR\s+(\d+)', content)
                minor_match = re.search(r'RENDERDOC_VERSION_MINOR\s+(\d+)', content)

                if major_match and minor_match:
                    return (major_match.group(1), minor_match.group(1))
            return (None, None)
        except Exception:
            return (None, None)

    def _get_git_commit_hash(self):
        """Get the current git commit hash of the project."""
        try:
            result = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=str(self.project_root),
                check=True,
            )
            return result.stdout.strip()
        except Exception:
            return "unknown"

    def _get_windows_sdk_version(self):
        """Get Windows SDK version from the configured Kits directory."""
        try:
            kits_root = self.windows_kits_root
            if kits_root.exists():
                include_dir = kits_root / "include"
                if include_dir.exists():
                    versions = [
                        item.name
                        for item in include_dir.iterdir()
                        if item.is_dir() and re.match(r'10\.0\.\d+\.\d+', item.name)
                    ]
                    if versions:
                        versions.sort(reverse=True)
                        return versions[0]

            # Fallback: read from project files
            project_file = self.project_root / "renderdoc" / "renderdoc.vcxproj"
            if project_file.exists():
                content = project_file.read_text()
                version_match = re.search(
                    r'<WindowsTargetPlatformVersion>([^<]+)</WindowsTargetPlatformVersion>',
                    content,
                )
                if version_match:
                    return version_match.group(1)

            return "Unknown"
        except Exception:
            return "Unknown"

    def _find_python_lib(self):
        """Find Python import library (.lib file)."""
        vi = sys.version_info
        major_minor = f"python{vi.major}{vi.minor}"

        possible_libs = [
            f"{major_minor}.lib",
            f"{major_minor}.dll.a",  # MinGW
            "python3.lib",  # Generic
        ]

        for lib_name in possible_libs:
            lib_path = self.python_libs / lib_name
            if lib_path.exists():
                return lib_path

        # Fallback: list available libs
        libs = list(self.python_libs.glob("python*.lib"))
        if libs:
            print(f"[!]  Warning: Using fallback library: {libs[0].name}")
            return libs[0]

        raise FileNotFoundError(
            f"Cannot find Python import library in {self.python_libs}. "
            f"Looked for: {possible_libs}"
        )

    def _find_msbuild(self):
        """Find MSBuild executable via vswhere (no hardcoded VS edition paths)."""
        try:
            result = subprocess.run(
                [
                    str(self.vswhere_path),
                    "-latest",
                    "-requires", "Microsoft.Component.MSBuild",
                    "-find", "MSBuild/**/Bin/MSBuild.exe",
                ],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                check=True,
            )
            if result.stdout.strip():
                return Path(result.stdout.strip().split('\n')[0])
        except Exception:
            pass

        raise FileNotFoundError(
            "Cannot find MSBuild.exe via vswhere. "
            f"Ensure Visual Studio is installed and vswhere is at: {self.vswhere_path}"
        )

    def _fix_project_warnings(self, project_path):
        """Fix project file to ignore Python 3.13+ deprecation warnings."""
        project_path = Path(project_path)
        auto_fix = self.cfg["auto_fixes"]["python_313_compat"]

        if not auto_fix["enabled"]:
            return

        ns_uri = auto_fix["xml_namespace"]
        flag = auto_fix["flag"]

        # Register the default namespace so ET.write() preserves the original
        # xmlns="..." form instead of emitting ns0: prefixes that break .vcxproj
        ET.register_namespace('', ns_uri)

        tree = ET.parse(project_path)
        root = tree.getroot()
        ns = {"ms": ns_uri}

        modified = False
        for item_group in root.findall(".//ms:ItemDefinitionGroup", ns):
            for cl_compile in item_group.findall("ms:ClCompile", ns):
                add_opts = cl_compile.find("ms:AdditionalOptions", ns)
                if add_opts is not None and add_opts.text:
                    if flag not in add_opts.text:
                        add_opts.text = add_opts.text.strip() + f" {flag}"
                        modified = True
                        print(f"   [E]  Updated {project_path.name} with {flag}")

        # Only rewrite the file if we actually changed something
        if modified:
            tree.write(str(project_path), encoding="utf-8", xml_declaration=True)

    def _build_breakpad_dependencies(self):
        """Build breakpad libraries required by renderdoc.dll in Release config."""
        breakpad_projects = self.cfg["project"].get("breakpad_projects", [])
        if not breakpad_projects:
            return True

        print("\n🔧 Building breakpad dependencies...")
        all_ok = True
        for rel_path in breakpad_projects:
            project_path = self.project_root / rel_path
            if not project_path.exists():
                print(f"   [!]  Warning: {rel_path} not found, skipping")
                continue

            cmd = [
                str(self.msbuild_path),
                str(project_path),
                f"-p:Configuration={self.config}",
                f"-p:Platform={self.platform}",
                f"-p:SolutionDir={self.project_root}\\",
                f"-p:PlatformToolset={self.platform_toolset}",
                "-v:minimal",
            ]

            result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")

            if result.returncode == 0:
                print(f"   [+] {project_path.name}")
            else:
                print(f"   [-] {project_path.name} failed")
                print(f"      {result.stderr[-500:]}")
                all_ok = False

        return all_ok

    def clean_build_artifacts(self):
        """Clean previous build artifacts."""
        print("\n🧹 Cleaning build artifacts...")

        if self.output_dir.exists():
            for pyd in self.output_dir.glob("*.pyd"):
                pyd.unlink()
                print(f"   Removed {pyd.name}")

            obj_dirs = [
                self.project_root / self.platform / self.config / "obj" / "pyrenderdoc_module",
                self.project_root / self.platform / self.config / "obj" / "qrenderdoc_module",
            ]
            for obj_dir in obj_dirs:
                if obj_dir.exists():
                    shutil.rmtree(obj_dir)
                    print(f"   Removed {obj_dir.relative_to(self.project_root)}")

    def build_module(self, project_path, module_name):
        """Build a single Python module."""
        print(f"\n🔨 Building {module_name}...")

        build_start = datetime.datetime.now()

        cmd = [
            str(self.msbuild_path),
            str(project_path),
            f"-p:Configuration={self.config}",
            f"-p:Platform={self.platform}",
            f"-p:SolutionDir={self.project_root}\\",
            f"-p:PlatformToolset={self.platform_toolset}",
            f"-p:PythonIncludeDir={self.python_include}",
            f"-p:PythonImportLib={self.python_import_lib}",
            "-v:minimal",
        ]

        result = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        build_end = datetime.datetime.now()
        build_duration = (build_end - build_start).total_seconds()

        self.build_results["modules"][module_name] = {
            "project": str(project_path),
            "success": result.returncode == 0,
            "duration": build_duration,
            "stdout": result.stdout,
            "stderr": result.stderr,
        }

        if result.returncode == 0:
            print(f"   [+] {module_name} built successfully ({build_duration:.1f}s)")
            return True
        else:
            print(f"   [-] {module_name} build failed")
            print("\n--- Error Output ---")
            print(result.stderr[-3000:])
            return False

    def copy_dependencies(self):
        """Copy required DLLs to output directory."""
        print("\n📦 Copying dependencies...")

        self.output_dir.mkdir(parents=True, exist_ok=True)

        # Copy the primary DLL (renderdoc.dll) from build output
        if self.renderdoc_dll.exists():
            dest = self.output_dir / "renderdoc.dll"
            shutil.copy2(self.renderdoc_dll, dest)
            size_mb = dest.stat().st_size / (1024 * 1024)
            print(f"   [+] renderdoc.dll ({size_mb:.1f} MB)")
            self.build_results["dependencies"]["renderdoc.dll"] = {
                "path": str(dest),
                "size_mb": size_mb,
                "copied": True,
            }
        else:
            print(f"   [!]  Warning: {self.renderdoc_dll} not found")
            self.build_results["dependencies"]["renderdoc.dll"] = {
                "path": str(self.renderdoc_dll),
                "copied": False,
            }

        # Check remaining required DLLs (already in output dir)
        for dll_name in self.required_dlls:
            if dll_name == "renderdoc.dll":
                continue
            dll_path = self.output_dir / dll_name
            if dll_path.exists():
                size_mb = dll_path.stat().st_size / (1024 * 1024)
                print(f"   [+] {dll_name} (already present, {size_mb:.1f} MB)")
                self.build_results["dependencies"][dll_name] = {
                    "path": str(dll_path),
                    "size_mb": size_mb,
                    "copied": False,
                }

    def copy_to_releases(self):
        """Copy built modules to python-releases directory."""
        print("\n[+] Copying to python-releases...")

        rd_major, rd_minor = self.renderdoc_version
        if rd_major is None or rd_minor is None:
            print("   [!]  Warning: Could not determine RenderDoc version")
            rd_version_str = "unknown"
        else:
            rd_version_str = f"{rd_major}.{rd_minor}"

        python_full_version_str = self._get_python_full_version().split()[0]

        release_dir_name = f"v{rd_version_str}_py{python_full_version_str}_{self.platform.lower()}"
        release_dir = self.project_root / "python-releases" / release_dir_name

        release_dir.mkdir(parents=True, exist_ok=True)

        copied_files = []
        for src_file in self.output_dir.iterdir():
            if src_file.is_file():
                dest_file = release_dir / src_file.name
                shutil.copy2(src_file, dest_file)
                size_mb = dest_file.stat().st_size / (1024 * 1024)
                print(f"   [+] {src_file.name} -> {release_dir_name}/ ({size_mb:.1f} MB)")
                copied_files.append(src_file.name)

        self._generate_stubs(release_dir, release_dir_name)

        # Generate README using file descriptions from config
        readme_content = f"""# RenderDoc Python Modules - {release_dir_name}

## Version Information

- **RenderDoc Version**: {rd_version_str}
- **Python Version**: {self.python_full_version}
- **Platform**: {self.platform} ({"Windows" if self.platform in ("x64", "x86") else "Unknown"})
- **Build Configuration**: {self.config}
- **Build Date**: {datetime.datetime.now().strftime("%Y-%m-%d")}

## Files

| File | Size | Description |
|------|------|-------------|
"""

        for filename in copied_files:
            filepath = release_dir / filename
            size_mb = filepath.stat().st_size / (1024 * 1024)
            description = self.file_descriptions.get(filename, "")
            readme_content += f"| `{filename}` | {size_mb:.1f} MB | {description} |\n"

        readme_content += f"""
## Usage

All files must be in the same directory when importing:

```python
import sys
sys.path.append(r'path\\to\\{release_dir_name}')

import renderdoc
import qrenderdoc
```

Or copy the `.pyd` files to your Python project directory.

## Type Stubs

This release includes Python type stubs (`.py` files with type annotations) in the `stubs/` directory for IDE autocomplete and type checking:

- `stubs/renderdoc/` - Type stubs for renderdoc module
- `stubs/qrenderdoc/` - Type stubs for qrenderdoc module

To use type stubs in your project, add the stubs directory to your IDE's Python path or to `MYPYPATH`.

## Requirements

- Windows 10/11 {self.platform}
- Python {self.python_version}.x
- Visual C++ Redistributable (usually pre-installed)

## Build Information

Built from RenderDoc source using MSBuild with Visual Studio 2022.
- **PlatformToolset**: {self.platform_toolset}
- **MSBuild Version**: {self.msbuild_version}

---

For build scripts and configuration, see the parent project's `.claude/skills/renderdoc-python-builder/` directory.
"""

        readme_path = release_dir / "README.md"
        readme_path.write_text(readme_content, encoding="utf-8")
        print(f"   [+] README.md -> {release_dir_name}/")

        self._generate_release_report(release_dir, release_dir_name, rd_version_str, copied_files)

        print(f"\n   Release directory: {release_dir.relative_to(self.project_root)}")
        return release_dir

    def _mirror_pymodules_for_stubs(self, target_dir):
        """Mirror built modules into RenderDoc stub-script compatibility paths."""
        target_dir = Path(target_dir)
        target_dir.mkdir(parents=True, exist_ok=True)
        for src_file in self.output_dir.iterdir():
            if src_file.is_file():
                dest_file = target_dir / src_file.name
                if src_file.resolve() == dest_file.resolve():
                    continue
                shutil.copy2(src_file, dest_file)

    def _prepare_stub_compatibility_paths(self):
        """Create paths expected by RenderDoc's historical regenerate_stubs.py scripts."""
        candidates = [
            self.project_root / self.platform / "Release" / "pymodules",
            self.project_root / self.platform / "Development" / "pymodules",
            self.project_root / f"{self.platform}Release" / "pymodules",
            self.project_root / f"{self.platform}Development" / "pymodules",
        ]
        for candidate in candidates:
            self._mirror_pymodules_for_stubs(candidate)

    def _generate_release_report(self, release_dir, release_dir_name, rd_version_str, copied_files):
        """Generate a detailed REPORT.md for the release directory."""

        total_size = sum((release_dir / f).stat().st_size for f in copied_files if (release_dir / f).exists())
        total_size_mb = total_size / (1024 * 1024)

        report_content = f"""# Build Report - {release_dir_name}

**Generated**: {datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")}
**Build Duration**: {(datetime.datetime.now() - self.build_start_time).total_seconds():.1f} seconds

---

## Release Information

| Property | Value |
|----------|-------|
| **Directory** | `{release_dir_name}/` |
| **RenderDoc Version** | {rd_version_str} |
| **Python Version** | {self.python_full_version} |
| **Platform** | {self.platform} |
| **Build Configuration** | {self.config} |
| **Total Size** | {total_size_mb:.1f} MB |
| **File Count** | {len(copied_files)} |
| **Git Commit** | `{self.git_commit_hash}` |

---

## Build Environment

### Python Configuration
| Property | Value |
|----------|-------|
| **Executable** | `{self.python_exe}` |
| **Version** | {self.python_version} |
| **Include Dir** | `{self.python_include}` |
| **Import Lib** | `{self.python_import_lib}` |

### Build Tools
| Tool | Version |
|------|---------|
| **MSBuild** | {self.msbuild_version} |
| **Platform Toolset** | {self.platform_toolset} |
| **Windows SDK** | {self.windows_sdk_version} |
| **Visual Studio** | {self.vs_info["display_name"]} |

### System Information
| Property | Value |
|----------|-------|
| **OS** | {self.system_info["os"]} |
| **OS Version** | {self.system_info["os_version"]} |
| **Architecture** | {self.system_info["machine"]} |

---

## Build Output

### Modules Built
| Module | Status |
|--------|--------|
| `pyrenderdoc_module` | {"Success" if self.build_results["modules"].get("pyrenderdoc_module", {}).get("success") else "Failed"} |
| `qrenderdoc_module` | {"Success" if self.build_results["modules"].get("qrenderdoc_module", {}).get("success") else "Failed"} |

### Files in Release

"""

        pyd_files = [f for f in copied_files if f.endswith('.pyd')]
        dll_files = [f for f in copied_files if f.endswith('.dll')]
        pdb_files = [f for f in copied_files if f.endswith('.pdb')]
        lib_files = [f for f in copied_files if f.endswith('.lib') or f.endswith('.exp')]

        if pyd_files:
            report_content += "#### Python Modules\n\n"
            for f in sorted(pyd_files):
                filepath = release_dir / f
                size_mb = filepath.stat().st_size / (1024 * 1024)
                report_content += f"- `{f}` ({size_mb:.1f} MB)\n"
            report_content += "\n"

        if dll_files:
            report_content += "#### Runtime Dependencies\n\n"
            for f in sorted(dll_files):
                filepath = release_dir / f
                size_mb = filepath.stat().st_size / (1024 * 1024)
                report_content += f"- `{f}` ({size_mb:.1f} MB)\n"
            report_content += "\n"

        if pdb_files:
            report_content += "#### Debug Symbols\n\n"
            for f in sorted(pdb_files):
                filepath = release_dir / f
                size_mb = filepath.stat().st_size / (1024 * 1024)
                report_content += f"- `{f}` ({size_mb:.1f} MB)\n"
            report_content += "\n"

        if lib_files:
            report_content += "#### Import Libraries & Export Files\n\n"
            for f in sorted(lib_files):
                filepath = release_dir / f
                size_kb = filepath.stat().st_size / 1024
                report_content += f"- `{f}` ({size_kb:.1f} KB)\n"
            report_content += "\n"

        report_content += "---\n\n## Test Results\n\n"

        for module_name, test_result in self.build_results.get("tests", {}).items():
            status_icon = "[+]" if test_result.get("status") == "Pass" else "[-]"
            status_text = test_result.get("status", "Unknown").upper()
            error_msg = f"\n**Error**: {test_result.get('error')}" if test_result.get("error") else ""
            report_content += f"### {module_name}\n\n{status_icon} **Status**: {status_text}{error_msg}\n\n"

        report_content += f"""---

## Build Parameters

### MSBuild Commands
```bash
# Build pyrenderdoc_module
{self.msbuild_path} \\
    {self.pyrenderdoc_project.relative_to(self.project_root)} \\
    -p:Configuration={self.config} \\
    -p:Platform={self.platform} \\
    -p:SolutionDir={self.project_root}\\ \\
    -p:PlatformToolset={self.platform_toolset} \\
    -p:PythonIncludeDir={self.python_include} \\
    -p:PythonLibraryDir={self.python_libs}

# Build qrenderdoc_module
{self.msbuild_path} \\
    {self.qrenderdoc_project.relative_to(self.project_root)} \\
    -p:Configuration={self.config} \\
    -p:Platform={self.platform} \\
    -p:SolutionDir={self.project_root}\\ \\
    -p:PlatformToolset={self.platform_toolset} \\
    -p:PythonIncludeDir={self.python_include} \\
    -p:PythonLibraryDir={self.python_libs}
```

### Project Configuration
- **Source Directory**: `{self.project_root}`
- **Output Directory**: `{self.output_dir.relative_to(self.project_root)}`
- **Release Directory**: `python-releases/{release_dir_name}/`

---

*This report was automatically generated by the RenderDoc Python Module Builder*
"""

        report_path = release_dir / "REPORT.md"
        report_path.write_text(report_content, encoding="utf-8")
        print(f"   [+] REPORT.md -> {release_dir_name}/")

    def _generate_stubs(self, release_dir, release_dir_name):
        """Generate Python type stubs for the built modules."""
        print("\n[+] Generating Python type stubs...")

        if not self.stubs_script.exists():
            message = f"Stubs generation script not found at {self.stubs_script}"
            if self.strict_stubs:
                raise FileNotFoundError(message)
            print(f"   [!]  Warning: {message}")
            return False

        stubs_output_dir = release_dir / "stubs"
        self._prepare_stub_compatibility_paths()
        env = os.environ.copy()
        env["PYTHONPATH"] = os.pathsep.join(
            [str(self.output_dir), env["PYTHONPATH"]]
            if env.get("PYTHONPATH")
            else [str(self.output_dir)]
        )
        env["PATH"] = os.pathsep.join([str(self.output_dir), env.get("PATH", "")])

        try:
            result = subprocess.run(
                [str(self.python_exe), str(self.stubs_script), str(stubs_output_dir)],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                cwd=self.stubs_script.parent,
                env=env,
                timeout=120,
            )

            if result.returncode == 0:
                renderdoc_stubs = list((stubs_output_dir / "renderdoc").glob("*.py")) if (stubs_output_dir / "renderdoc").exists() else []
                qrenderdoc_stubs = list((stubs_output_dir / "qrenderdoc").glob("*.py")) if (stubs_output_dir / "qrenderdoc").exists() else []
                total_stubs = len(renderdoc_stubs) + len(qrenderdoc_stubs)

                print(f"   [+] Generated {total_stubs} stub files")
                print(f"      - renderdoc: {len(renderdoc_stubs)} files")
                print(f"      - qrenderdoc: {len(qrenderdoc_stubs)} files")

                self.build_results["stubs"] = {
                    "renderdoc_count": len(renderdoc_stubs),
                    "qrenderdoc_count": len(qrenderdoc_stubs),
                    "total_count": total_stubs,
                    "output_dir": str(stubs_output_dir.relative_to(self.project_root)),
                }
                if self.strict_stubs and (not renderdoc_stubs or not qrenderdoc_stubs):
                    raise RuntimeError(
                        "Stubs generation completed but did not produce both "
                        "renderdoc and qrenderdoc stub files"
                    )
                return True
            else:
                message = (
                    f"Stubs generation failed with exit code {result.returncode}\n"
                    f"stdout:\n{result.stdout}\n"
                    f"stderr:\n{result.stderr}"
                )
                if self.strict_stubs:
                    raise RuntimeError(message)
                print(f"   [!]  {message}")
                return False

        except subprocess.TimeoutExpired:
            message = "Stubs generation timed out after 2 minutes"
            if self.strict_stubs:
                raise RuntimeError(message)
            print(f"   [!]  {message}")
            return False
        except Exception as e:
            if self.strict_stubs:
                raise
            print(f"   [!]  Error generating stubs: {e}")
            return False

    def test_modules(self):
        """Test if modules can be imported."""
        print("\n🧪 Testing modules...")

        sys.path.insert(0, str(self.output_dir))

        results = {}

        try:
            import renderdoc

            results["renderdoc"] = {"status": "Pass", "error": None}
            print("   [+] renderdoc module loaded successfully")
            if hasattr(renderdoc, "CaptureFile"):
                print("      - Has CaptureFile attribute")
        except Exception as e:
            results["renderdoc"] = {"status": "Failed", "error": str(e)}
            print(f"   [-] renderdoc: {e}")

        try:
            import qrenderdoc

            results["qrenderdoc"] = {"status": "Pass", "error": None}
            print("   [+] qrenderdoc module loaded successfully")
        except Exception as e:
            results["qrenderdoc"] = {"status": "Failed", "error": str(e)}
            print(f"   [-] qrenderdoc: {e}")

        self.build_results["tests"] = results

        return results

    def generate_report(self):
        """Generate a detailed build report in markdown format."""
        build_end_time = datetime.datetime.now()
        build_duration = (build_end_time - self.build_start_time).total_seconds()

        report_path = self.project_root / "REPORT_GLOBAL.md"

        report_content = f"""# RenderDoc Python Module Build Report

**Generated**: {self.build_start_time.strftime("%Y-%m-%d %H:%M:%S")}
**Build Duration**: {build_duration:.1f} seconds
**Status**: {"[+] SUCCESS" if all(m["success"] for m in self.build_results["modules"].values()) else "[-] FAILED"}
**Source Commit**: `{self.git_commit_hash}`

---

## Build Environment

### Python Configuration
| Property | Value |
|----------|-------|
| **Python Executable** | `{self.python_exe}` |
| **Python Version** | {self.python_version} |
| **Full Version** | `{self.python_full_version}` |
| **Include Directory** | `{self.python_include}` |
| **Libs Directory** | `{self.python_libs}` |
| **Import Library** | `{self.python_import_lib}` |

### Build Tools
| Tool | Version |
|------|---------|
| **MSBuild** | {self.msbuild_version} |
| **MSBuild Path** | `{self.msbuild_path}` |
| **Platform Toolset** | {self.platform_toolset} |
| **Windows SDK** | {self.windows_sdk_version} |

### Visual Studio
| Property | Value |
|----------|-------|
| **Display Name** | {self.vs_info["display_name"]} |
| **Installation Path** | `{self.vs_info["installation_path"]}` |

### System Information
| Property | Value |
|----------|-------|
| **Operating System** | {self.system_info["os"]} |
| **OS Version** | {self.system_info["os_version"]} |
| **OS Release** | {self.system_info["os_release"]} |
| **Architecture** | {self.system_info["machine"]} |
| **Processor** | {self.system_info["processor"]} |
| **Python Implementation** | {self.system_info["python_implementation"]} |

---

## Build Configuration

| Property | Value |
|----------|-------|
| **Configuration** | {self.config} |
| **Platform** | {self.platform} |
| **RenderDoc Version** | {self.renderdoc_version} |
| **Git Commit** | `{self.git_commit_hash}` |
| **Project Root** | `{self.project_root}` |

---

## Built Modules

### pyrenderdoc_module
| Property | Value |
|----------|-------|
| **Project File** | `{self.pyrenderdoc_project.relative_to(self.project_root)}` |
| **Status** | {"[+] SUCCESS" if self.build_results["modules"].get("pyrenderdoc_module", {}).get("success") else "[-] FAILED"} |
| **Build Time** | {self.build_results["modules"].get("pyrenderdoc_module", {}).get("duration", 0):.1f}s |

### qrenderdoc_module
| Property | Value |
|----------|-------|
| **Project File** | `{self.qrenderdoc_project.relative_to(self.project_root)}` |
| **Status** | {"[+] SUCCESS" if self.build_results["modules"].get("qrenderdoc_module", {}).get("success") else "[-] FAILED"} |
| **Build Time** | {self.build_results["modules"].get("qrenderdoc_module", {}).get("duration", 0):.1f}s |

---

## Output Files

### Python Extension Modules (.pyd)
"""

        for pyd_file in sorted(self.output_dir.glob("*.pyd")):
            size_mb = pyd_file.stat().st_size / (1024 * 1024)
            report_content += f"| **{pyd_file.name}** | {size_mb:.2f} MB | `{pyd_file.relative_to(self.project_root)}` |\n"

        report_content += """
### Dependencies (DLLs)
"""

        for dll_name, dll_info in self.build_results.get("dependencies", {}).items():
            if dll_info.get("copied") or dll_info.get("size_mb"):
                size = dll_info.get("size_mb", 0)
                status = "[+] Copied" if dll_info.get("copied") else "Present"
                report_content += f"| **{dll_name}** | {size:.2f} MB | {status} |\n"

        report_content += f"""

---

## Test Results

### Module Import Tests

"""

        for module_name, test_result in self.build_results.get("tests", {}).items():
            status_icon = "[+]" if test_result.get("status") == "Pass" else "[-]"
            status_text = test_result.get("status", "Unknown").upper()
            error_msg = f"\n**Error**: {test_result.get('error')}" if test_result.get("error") else ""
            report_content += f"#### {module_name}\n\n{status_icon} **Status**: {status_text}{error_msg}\n\n"

        report_content += f"""
---

## Output Directory

```
{self.output_dir.relative_to(self.project_root)}
```

### Usage

```python
import sys
sys.path.insert(0, '{self.output_dir.relative_to(self.project_root)}')

import renderdoc
import qrenderdoc
```

---

## Build Parameters

### MSBuild Command
```bash
{self.msbuild_path} \\
    <project_file> \\
    -p:Configuration={self.config} \\
    -p:Platform={self.platform} \\
    -p:SolutionDir={self.project_root}\\ \\
    -p:PlatformToolset={self.platform_toolset} \\
    -p:PythonIncludeDir={self.python_include} \\
    -p:PythonImportLib={self.python_import_lib} \\
    -v:minimal
```

### Environment Variables
- `PYTHON_EXE={self.python_exe}`
- `PYTHON_VERSION={self.python_version}`
- `PYTHON_INCLUDE_DIR={self.python_include}`
- `PYTHON_LIBS_DIR={self.python_libs}`
- `MSBUILD_PATH={self.msbuild_path}`

---

## Notes

### Python 3.13+ Compatibility
The build process automatically modifies project files to ignore Python 3.13+ deprecation warnings (`{self.cfg["auto_fixes"]["python_313_compat"]["flag"]}` compiler option).

### Modified Files
- `{self.pyrenderdoc_project.relative_to(self.project_root)}`
- `{self.qrenderdoc_project.relative_to(self.project_root)}`

### Compilation Flags
```
/wd4100 /wd4512 {self.cfg["auto_fixes"]["python_313_compat"]["flag"]}
```

---

*Report generated by RenderDoc Python Module Builder*
*Configuration: assets/config.json*
"""

        report_path.write_text(report_content, encoding="utf-8")

        print(f"\n[R] Build report saved to: {report_path}")
        return report_path

    def build(self):
        """Execute the full build process."""
        print("\n" + "=" * 60)
        print("🚀 Starting RenderDoc Python Module Build")
        print("=" * 60)

        # Step 1: Fix project files
        print("\n📝 Step 1: Fixing project files...")
        self._fix_project_warnings(self.pyrenderdoc_project)
        self._fix_project_warnings(self.qrenderdoc_project)

        # Step 2: Clean
        self.clean_build_artifacts()

        # Step 2.5: Build breakpad dependencies (needed for Release linker)
        self._build_breakpad_dependencies()

        # Step 3: Build pyrenderdoc_module
        success = self.build_module(self.pyrenderdoc_project, "pyrenderdoc_module")
        if not success:
            return False

        # Step 4: Build qrenderdoc_module
        success = self.build_module(self.qrenderdoc_project, "qrenderdoc_module")
        if not success:
            return False

        # Step 5: Copy dependencies
        self.copy_dependencies()

        # Step 6: Test
        results = self.test_modules()

        # Step 7: Copy to releases
        self.copy_to_releases()

        # Step 8: Generate Report
        print("\n[R] Generating build report...")
        report_path = self.generate_report()

        # Summary
        print("\n" + "=" * 60)
        print("📊 Build Summary")
        print("=" * 60)
        print(f"   Python Version: {self.python_version}")
        print(f"   Output Directory: {self.output_dir}")
        print(f"   Build Report: {report_path.relative_to(self.project_root)}")
        print(f"   Git Commit: {self.git_commit_hash[:12]}")
        print(f"\n   Modules:")

        for pyd in self.output_dir.glob("*.pyd"):
            size_mb = pyd.stat().st_size / (1024 * 1024)
            print(f"      - {pyd.name} ({size_mb:.1f} MB)")

        print(f"\n   Test Results:")
        for module, result in results.items():
            status = result.get("status", result) if isinstance(result, dict) else result
            print(f"      - {module}: {status}")

        if all(isinstance(r, dict) and r.get("status") == "Pass" for r in results.values()):
            print("\n   [OK] All tests passed!")
            return True
        else:
            print("\n   [!]  Some tests failed")
            return False


def main():
    parser = argparse.ArgumentParser(
        description="Build RenderDoc Python modules for a specific Python version"
    )
    parser.add_argument(
        "--python-version",
        help="Python version (e.g., 3.13). Auto-detected if not specified.",
    )
    parser.add_argument(
        "--config",
        default="Development",
        choices=["Development", "Release"],
        help="Build configuration (default: Development)",
    )
    parser.add_argument(
        "--platform",
        default="x64",
        choices=["x64", "x86"],
        help="Target platform (default: x64)",
    )
    parser.add_argument(
        "--project-root",
        help="Path to the RenderDoc source checkout. Auto-detected if omitted.",
    )
    parser.add_argument(
        "--strict-stubs",
        action="store_true",
        help="Fail the build if per-version renderdoc/qrenderdoc stubs are not generated.",
    )

    args = parser.parse_args()

    try:
        builder = RenderDocPyBuilder(
            python_version=args.python_version,
            config=args.config,
            platform_arch=args.platform,
            project_root=args.project_root,
            strict_stubs=args.strict_stubs,
        )
        success = builder.build()
        sys.exit(0 if success else 1)

    except Exception as e:
        print(f"\n[-] Error: {e}")
        import traceback

        traceback.print_exc()
        sys.exit(1)


if __name__ == "__main__":
    main()
