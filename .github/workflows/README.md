# GitHub Actions Workflows

This directory contains GitHub Actions workflows for automating the build and release process of the WebRTC project.

## Workflows

### 1. `build.yml` - Main Build Workflow
**Triggers:** Push to `main` or `develop` branches, Pull Requests to `main` or `develop`

**Platforms:** Linux (Ubuntu), macOS, Windows

**Features:**
- Builds both debug and release configurations
- Uses matrix strategy for parallel builds across platforms
- Caches WebRTC source code for faster builds
- Uploads build artifacts with 7-day retention
- Supports the whillats extension

### 2. `pr-validation.yml` - Pull Request Validation
**Triggers:** Pull Requests to `main` or `develop` branches

**Purpose:** Fast validation for PRs without full builds

**Features:**
- Validates build script syntax
- Checks for required files
- Quick depot_tools setup test
- Optional macOS testing (triggered by `[macos]` in PR title or `test-macos` label)
- Lightweight validation to provide quick feedback

### 3. `release.yml` - Release Build Workflow
**Triggers:** Published releases, Tags starting with `v*`

**Platforms:** Linux (Ubuntu), macOS

**Features:**
- Builds release configuration only
- Packages artifacts into compressed archives
- Automatically uploads to GitHub Releases
- Optimized for production builds

## Configuration

### Environment Requirements
- **Python:** 3.12 (automatically installed)
- **depot_tools:** Automatically cloned and configured
- **System Dependencies:** 
  - Linux: build-essential, pkg-config, libasound2-dev, libgtk-3-dev, libxtst6, ninja-build
  - macOS: ninja, pkg-config (via Homebrew)
  - Windows: Visual Studio Build Tools, MSBuild

### Caching Strategy
- WebRTC source code is cached using `actions/cache@v3`
- Cache key includes OS and DEPS file hash
- Significantly reduces build times for subsequent runs

### Artifacts
- **Debug/Release builds:** Uploaded with platform-specific naming
- **Release packages:** Compressed tar.gz files for distribution
- **Retention:** 7 days for regular builds, permanent for releases

## Usage

### Triggering Builds
1. **Automatic:** Push to main/develop or create PR
2. **Manual:** Use GitHub Actions UI to manually trigger workflows
3. **Release:** Create a new release or push a version tag

### macOS Testing in PRs
To trigger macOS testing in a PR:
- Add `[macos]` to the PR title, OR
- Add the `test-macos` label to the PR

### Monitoring
- View build status in the Actions tab
- Download artifacts from completed workflow runs
- Check logs for debugging build issues

## Customization

### Adding New Platforms
1. Add new OS to the matrix in `build.yml`
2. Add platform-specific dependency installation steps
3. Update artifact upload paths for the new platform

### Modifying Build Types
- Edit the `build_type` matrix in workflow files
- Ensure the build script supports the new build types
- Update artifact naming accordingly

## Troubleshooting

### Common Issues
1. **Cache misses:** Check if DEPS file changed significantly
2. **Build timeouts:** Adjust timeout values or optimize build script
3. **Dependency failures:** Verify system dependency installation steps
4. **Windows builds:** May require additional Visual Studio components

### Debugging
- Enable debug logging by setting `ACTIONS_STEP_DEBUG=true`
- Check individual step logs for detailed error information
- Review build script output for WebRTC-specific issues
