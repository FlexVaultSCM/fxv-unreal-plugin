const fs = require('fs');
const path = require('path');

const repoRoot = path.resolve(__dirname, '..', '..');

// 1. Read plugin version from FlexVaultSourceControl.uplugin
const upluginPath = path.join(repoRoot, 'FlexVaultSourceControl', 'FlexVaultSourceControl.uplugin');
if (!fs.existsSync(upluginPath)) {
  console.error('Error: uplugin not found at ' + upluginPath);
  process.exit(1);
}
const uplugin = JSON.parse(fs.readFileSync(upluginPath, 'utf8'));
const pluginVersion = uplugin.VersionName;
if (!pluginVersion) {
  console.error('Error: missing VersionName in ' + upluginPath);
  process.exit(1);
}

// 2. Read CLI compatibility range from FlexVaultSourceControlWorkerHelper.h
const helperHeaderPath = path.join(
  repoRoot,
  'FlexVaultSourceControl',
  'Source',
  'FlexVaultSourceControl',
  'Private',
  'Workers',
  'FlexVaultSourceControlWorkerHelper.h'
);
if (!fs.existsSync(helperHeaderPath)) {
  console.error('Error: helper header not found at ' + helperHeaderPath);
  process.exit(1);
}
const helperHeader = fs.readFileSync(helperHeaderPath, 'utf8');
const compatNamespaceMatch = helperHeader.match(/namespace\s+FlexVaultCliCompatibility\s*\{([\s\S]*?)\}/);
if (!compatNamespaceMatch) {
  console.error('Error: Could not find namespace FlexVaultCliCompatibility in helper header.');
  process.exit(1);
}
const compatBlock = compatNamespaceMatch[1];
const minMatch = compatBlock.match(/MinMajor\s*=\s*(\d+)\s*,\s*MinMinor\s*=\s*(\d+)\s*,\s*MinPatch\s*=\s*(\d+)/);
const maxMatch = compatBlock.match(/MaxMajor\s*=\s*(\d+)\s*,\s*MaxMinor\s*=\s*(\d+)\s*,\s*MaxPatch\s*=\s*(\d+)/);

if (!minMatch || !maxMatch) {
  console.error('Error: Could not parse FlexVaultCliCompatibility min/max bounds in helper header.');
  process.exit(1);
}

const cliMin = `${minMatch[1]}.${minMatch[2]}.${minMatch[3]}`;
const cliMax = `${maxMatch[1]}.${maxMatch[2]}.${maxMatch[3]}`;

const semverRegex = /^\d+\.\d+\.\d+$/;
if (!semverRegex.test(cliMin) || !semverRegex.test(cliMax)) {
  console.error(`Error: Parsed CLI compatibility bounds ('${cliMin}', '${cliMax}') are not valid semver.`);
  process.exit(1);
}

// 3. Resolve tag and clean version
let tag = process.env.RELEASE_TAG || `v${pluginVersion}`;
const tagArgIndex = process.argv.indexOf('--tag');
if (tagArgIndex !== -1 && tagArgIndex + 1 < process.argv.length) {
  tag = process.argv[tagArgIndex + 1];
}
const cleanVersion = tag.replace(/^v/, '');

if (cleanVersion !== pluginVersion) {
  console.error(`Error: Tag version '${cleanVersion}' does not match uplugin VersionName '${pluginVersion}'.`);
  process.exit(1);
}

const outputPath = path.join(repoRoot, 'version.json');
const manifest = {
  name: 'unreal',
  plugin_version: cleanVersion,
  tag: tag,
  cli_compatibility: {
    min: cliMin,
    max: cliMax
  },
  assets: {
    plugin_zip: `FlexVaultSourceControl-${cleanVersion}.zip`,
    plugin_zip_latest: 'FlexVaultSourceControl.zip',
    plugin_fab_zip: `FlexVaultSourceControl-${cleanVersion}-Fab.zip`
  }
};

fs.writeFileSync(outputPath, JSON.stringify(manifest, null, 2) + '\n', 'utf8');
console.log(`Generated ${outputPath}:`);
console.log(JSON.stringify(manifest, null, 2));
