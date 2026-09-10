const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');

const inputVersion = process.argv[2];
if (!inputVersion) {
  console.error('Usage: node set-version.js <version-or-tag>');
  process.exit(1);
}

// Strip leading 'v' if provided, e.g. "v0.2.1" -> "0.2.1"
const cleanVersion = inputVersion.trim().replace(/^v/, '');

// Validate semver format (major.minor.patch[-prerelease])
const semverRegex = /^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/;
if (!semverRegex.test(cleanVersion)) {
  console.error(`Error: '${inputVersion}' is not a valid semantic version.`);
  process.exit(1);
}

console.log(`Setting unreal plugin version to: ${cleanVersion}`);

// 1. Update FlexVaultSourceControl/FlexVaultSourceControl.uplugin
const upluginPath = path.join(repoRoot, 'FlexVaultSourceControl', 'FlexVaultSourceControl.uplugin');
if (fs.existsSync(upluginPath)) {
  const uplugin = JSON.parse(fs.readFileSync(upluginPath, 'utf8'));
  uplugin.VersionName = cleanVersion;
  const majorPart = parseInt(cleanVersion.split('.')[0], 10);
  if (!isNaN(majorPart) && majorPart > 0) {
    uplugin.Version = majorPart;
  }
  fs.writeFileSync(upluginPath, JSON.stringify(uplugin, null, '\t') + '\n', 'utf8');
  console.log(`Updated FlexVaultSourceControl.uplugin (VersionName: ${cleanVersion}, Version: ${uplugin.Version})`);
} else {
  console.error(`Error: uplugin file not found at ${upluginPath}`);
  process.exit(1);
}

// 2. Update .github/release-please-manifest.json
const manifestPath = path.join(repoRoot, '.github', 'release-please-manifest.json');
if (fs.existsSync(manifestPath)) {
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  manifest['.'] = cleanVersion;
  fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n', 'utf8');
  console.log(`Updated release-please-manifest.json -> ${cleanVersion}`);
}

console.log(`Successfully synced unreal plugin version to ${cleanVersion}.`);
