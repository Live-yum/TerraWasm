"use strict";

const fs = require("node:fs");
const path = require("node:path");

const TESTS_DIR = path.resolve(__dirname, "..");
const PRIMARY_WORLD_PATH = path.join(TESTS_DIR, "pixel_art_output.wld");
const SECONDARY_WORLD_PATH = path.join(TESTS_DIR, "pixel_art_from_jpeg.wld");
const TRUNCATED_FIXTURE_DIR = path.join(TESTS_DIR, "fixtures", "truncated");
const CORRUPTED_FIXTURE_DIR = path.join(TESTS_DIR, "fixtures", "corrupted");
const JPEG_PATH = path.join(TESTS_DIR, "fixtures", "pixel-art-input.jpg");

function existingPaths(paths) {
  return paths.filter((filePath) => fs.existsSync(filePath));
}

function getWorldFixturePaths() {
  const paths = existingPaths([PRIMARY_WORLD_PATH, SECONDARY_WORLD_PATH]);
  if (!paths.length) {
    throw new Error("No checked-in world fixtures are available under tests/");
  }
  return paths;
}

function getPrimaryWorldPath() {
  return getWorldFixturePaths()[0];
}

function readFixtureDirectory(directory) {
  if (!fs.existsSync(directory)) return [];
  return fs.readdirSync(directory)
    .filter((name) => !name.endsWith(".md"))
    .sort((left, right) => left.localeCompare(right))
    .map((name) => path.join(directory, name));
}

module.exports = {
  CORRUPTED_FIXTURE_DIR,
  JPEG_PATH,
  PRIMARY_WORLD_PATH,
  SECONDARY_WORLD_PATH,
  TESTS_DIR,
  TRUNCATED_FIXTURE_DIR,
  getPrimaryWorldPath,
  getWorldFixturePaths,
  readFixtureDirectory,
};
