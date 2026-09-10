const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');

// 1. Read FlexVaultSourceControl/FlexVaultSourceControl.uplugin (single source of truth)
const upluginPath = path.join(repoRoot, 'FlexVaultSourceControl', 'FlexVaultSourceControl.uplugin');
if (!fs.existsSync(upluginPath)) {
  console.error('Error: uplugin file not found at ' + upluginPath);
  process.exit(1);
}

const uplugin = JSON.parse(fs.readFileSync(upluginPath, 'utf8'));
const pluginVersion = uplugin.VersionName;
if (!pluginVersion) {
  console.error('Error: FlexVaultSourceControl.uplugin missing "VersionName" field.');
  process.exit(1);
}

const semverRegex = /^\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?$/;
if (!semverRegex.test(pluginVersion)) {
  console.error(`Error: FlexVaultSourceControl.uplugin VersionName '${pluginVersion}' is not a valid semantic version.`);
  process.exit(1);
}

console.log(`[Version Check] FlexVaultSourceControl.uplugin VersionName: ${pluginVersion} (Version: ${uplugin.Version})`);

// 2. Validate .github/release-please-manifest.json
const manifestPath = path.join(repoRoot, '.github', 'release-please-manifest.json');
if (fs.existsSync(manifestPath)) {
  const manifest = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  const manifestVersion = manifest['.'];
  if (manifestVersion && manifestVersion !== pluginVersion) {
    console.error(`Error: Release manifest version (${manifestVersion}) does not match uplugin VersionName (${pluginVersion}).`);
    process.exit(1);
  }
  console.log(`[Version Check] release manifest version matches: ${manifestVersion}`);
}

// 3. If --tag argument is provided, validate git release tag matches package version
const tagIndex = process.argv.indexOf('--tag');
if (tagIndex !== -1 && tagIndex + 1 < process.argv.length) {
  const tag = process.argv[tagIndex + 1];
  const cleanTag = tag.replace(/^v/, '');
  if (cleanTag !== pluginVersion) {
    console.error(`Error: Release tag '${tag}' (${cleanTag}) does not match uplugin VersionName (${pluginVersion})!`);
    process.exit(1);
  }
  console.log(`[Version Check] Release tag matches uplugin VersionName: ${tag} == ${pluginVersion}`);
}

console.log('All version checks passed successfully.');
