#!/usr/bin/env python3
"""Build script to create direct.xcframework for both iOS device and simulator."""

import argparse
import os
import shutil
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
SRC_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, '..'))

def run_command(cmd, cwd=None, use_shell=True):
    """Run a command and check for errors."""
    # Add depot_tools to PATH for shell commands
    env = os.environ.copy()
    depot_tools_path = os.path.expanduser("~/Public/depot_tools")
    if depot_tools_path not in env.get('PATH', ''):
        env['PATH'] = f"{depot_tools_path}:{env.get('PATH', '')}"
    
    cmd_str = ' '.join(cmd) if isinstance(cmd, list) else cmd
    print(f"Running: {cmd_str}")
    
    result = subprocess.run(cmd_str, cwd=cwd or SRC_DIR, capture_output=True, text=True, shell=True, env=env)
    
    if result.returncode != 0:
        print(f"Command failed with exit code {result.returncode}")
        if result.stderr:
            print(f"stderr: {result.stderr}")
        if result.stdout:
            print(f"stdout: {result.stdout}")
        sys.exit(1)
    else:
        # Show output for successful commands too
        if result.stdout:
            print(result.stdout)
    return result

def build_framework(target_environment, config, use_speech_audio):
    """Build framework for a specific target environment."""
    print(f"\n=== Building directcall for {target_environment} ===")
    
    # Set build directory based on target environment
    if target_environment == "device":
        build_dir = "out/ios_arm64"
    else:  # simulator
        build_dir = "out/ios_sim_arm64"
    
    # Use your exact working gn args
    gn_args = [
        f'target_environment="{target_environment}"',
        'target_os="ios"',
        'target_cpu="arm64"',
        'ios_deployment_target="16.4.0"',
        f'is_debug={"true" if config == "debug" else "false"}',
        'rtc_include_opus=true',
        'rtc_build_examples=true',
        'rtc_enable_symbol_export=true',
        'mac_deployment_target="15.0"',
        'mac_min_system_version="15.0"',
        f'rtc_use_speech_audio_devices={"true" if use_speech_audio else "false"}',
    ]
    
    # Generate build files
    args_string = ' '.join(gn_args)
    gn_cmd = f'gn gen {build_dir} --args=\'{args_string}\''
    run_command(gn_cmd)
    
    # Build directcall
    ninja_cmd = f'ninja -C {build_dir} directcall'
    run_command(ninja_cmd)

def create_xcframework(device_framework, simulator_framework, output_path):
    """Create XCFramework from device and simulator frameworks."""
    print(f"\n=== Creating XCFramework ===")
    
    if os.path.exists(output_path):
        shutil.rmtree(output_path)
    
    cmd = [
        'xcodebuild', '-create-xcframework',
        '-framework', device_framework,
        '-framework', simulator_framework,
        '-output', output_path
    ]
    
    run_command(cmd)
    print(f"Successfully created XCFramework at {output_path}")

def main():
    parser = argparse.ArgumentParser(description='Build direct.xcframework')
    parser.add_argument('--config', choices=['debug', 'release'], default='debug',
                       help='Build configuration (default: debug to match your commands)')
    parser.add_argument('--clean', action='store_true',
                       help='Clean build directories before building')
    parser.add_argument('--use-speech-audio', action='store_true', default=False,
                       help='Enable speech audio devices (rtc_use_speech_audio_devices=true)')
    
    args = parser.parse_args()
    
    if args.clean:
        # Clean the build directories
        for build_dir in ['out/ios_arm64', 'out/ios_sim_arm64']:
            if os.path.exists(build_dir):
                print(f"Cleaning {build_dir}")
                shutil.rmtree(build_dir)
    
    # Build for device and simulator
    build_framework('device', args.config, args.use_speech_audio)
    build_framework('simulator', args.config, args.use_speech_audio)
    
    # Framework paths - look for framework in the build output
    device_framework = "out/ios_arm64/direct.framework"
    simulator_framework = "out/ios_sim_arm64/direct.framework"
    
    # Verify frameworks exist
    if not os.path.exists(device_framework):
        print(f"Error: Device framework not found at {device_framework}")
        # List what's actually in the build directory
        device_dir = "out/ios_arm64"
        if os.path.exists(device_dir):
            print(f"Contents of {device_dir}:")
            for item in os.listdir(device_dir):
                print(f"  {item}")
        sys.exit(1)
    
    if not os.path.exists(simulator_framework):
        print(f"Error: Simulator framework not found at {simulator_framework}")
        # List what's actually in the build directory
        sim_dir = "out/ios_sim_arm64"
        if os.path.exists(sim_dir):
            print(f"Contents of {sim_dir}:")
            for item in os.listdir(sim_dir):
                print(f"  {item}")
        sys.exit(1)
    
    # Create XCFramework in out directory
    os.makedirs("out", exist_ok=True)
    xcframework_path = "out/direct.xcframework"
    create_xcframework(device_framework, simulator_framework, xcframework_path)
    
    print(f"\n✅ Build complete! XCFramework available at: {xcframework_path}")

if __name__ == '__main__':
    main()
