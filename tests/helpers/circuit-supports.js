'use strict';
const { makeCircuitWorld } = require('./circuit-world');

// Independent sparse scene with the shapes/anchors written out explicitly.
// Each group has a known footprint; ordinary cave decor crosses its wire path.
function scene() {
  const cells = new Map(), geometry = [], geometryKeys = new Set();
  const put = (x, y, tile) => cells.set(`${x},${y}`, { ...cells.get(`${x},${y}`), x, y, ...tile });
  function object(type, x, y, width, height, anchor, wired = [0, 0, 1], attrs = {}) {
    for (let dx = 0; dx < width; dx++) for (let dy = 0; dy < height; dy++) {
      const fx = (attrs.fx || 0) + dx * 18, fy = (attrs.fy || 0) + dy * 18;
      put(x + dx, y + dy, { ...attrs, type, fx, fy, wires: dx === wired[0] && dy === wired[1] ? wired[2] : 0 });
      const key = `${type}:${fx}:${fy}`;
      if (!geometryKeys.has(key)) { geometryKeys.add(key); geometry.push([type, (fx | fy << 16) >>> 0, (dx | dy << 8 | width << 16 | height << 24) >>> 0, anchor]); }
    }
  }
  return { cells, geometry, put, object };
}
function fixture() {
  const { cells, geometry, put, object } = scene();
  for (let x = 2; x <= 12; x++) put(x, 6, { type: 1, paint: 3, wall: 3, wallPaint: 6, wires: 1 });
  for (let y = 3; y <= 9; y++) put(8, y, { wires: y === 6 ? 3 : 2 });
  object(135, 4, 6, 1, 1, 1); put(4, 7, { type: 54, paint: 7 });
  object(137, 12, 6, 1, 1, 0);
  // Pots and stalactites do not join circuits or demand complete geometry.
  put(9, 6, { type: 28, fx: 72, fy: 72, wires: 1 });
  put(10, 6, { type: 165, fx: 90, fy: 18, wires: 1 });
  put(5, 6, { liquid: 255, liquidType: 1 });

  // An ordinary boulder trap: only its active-stone release tile is wired.
  for (let x = 2; x <= 10; x++) put(x, 15, { wires: 1 });
  put(10, 15, { type: 130 }); put(11, 15, { type: 1 });
  object(138, 10, 13, 2, 2, 1, [-1, -1, 0]);

  object(33, 20, 10, 1, 1, 14);
  object(14, 20, 11, 3, 2, 1, [-1, -1, 0]);
  for (let x = 20; x <= 22; x++) put(x, 13, { type: 54 });

  object(10, 2, 20, 1, 3, 3, [0, 1, 2]);
  put(2, 19, { type: 54 }); put(2, 23, { type: 54 }); put(3, 21, { type: 1 });
  object(136, 8, 21, 1, 1, 5, [0, 0, 4]);
  put(7, 21, { type: 1 }); put(9, 21, { type: 1 });
  object(132, 12, 20, 2, 2, 7, [0, 0, 1], { wall: 2, wallPaint: 9 });
  object(34, 20, 20, 3, 3, 4, [1, 1, 8]);
  for (let x = 20; x <= 22; x++) put(x, 19, { type: 1 });
  object(209, 28, 20, 4, 3, 17, [0, 0, 2]);
  for (let x = 28; x <= 31; x++) put(x, 23, { type: 54 });
  object(21, 28, 4, 2, 2, 1);
  put(28, 6, { type: 54 }); put(29, 6, { type: 54 });
  object(4, 36, 12, 1, 1, 13, [0, 0, 2], { fx: 22, wall: 2, wallPaint: 9, paint: 5 });
  put(37, 12, { type: 1 });
  return { world: makeCircuitWorld([...cells.values()], 40, 32, 326), geometry };
}

// Source-specific alternatives independently exercise the material checks,
// including cases where a visually adjacent block is not a legal anchor.
function anchorFixture() {
  const { cells, geometry, put, object } = scene();
  // Wiring excludes the outer two world rows; the wired interior part of a
  // complete object still exercises an anchor just beyond the world edge.
  object(34, 0, 0, 3, 3, 4, [2, 2, 1]);
  object(10, 24, 0, 1, 3, 3, [0, 2, 1]); put(24, 3, { type: 1 });
  object(4, 5, 5, 1, 1, 13, [0, 0, 1], { fx: 22 });
  for (let y = 4; y <= 6; y++) put(4, y, { type: 5 });
  object(4, 12, 5, 1, 1, 13, [0, 0, 1], { fx: 22 });
  put(11, 5, { type: 5 }); // A single trunk tile is insufficient.
  object(136, 18, 5, 1, 1, 5); put(17, 5, { type: 124 });
  object(42, 24, 5, 1, 2, 2); put(24, 4, { type: 427 });
  object(42, 30, 5, 1, 2, 2); put(30, 4, { type: 435 });
  object(138, 36, 4, 2, 2, 1, [-1, -1, 0]);
  put(36, 6, { type: 130, wires: 2 }); // Either foot can hold the boulder.
  object(21, 42, 4, 2, 2, 1);
  object(14, 42, 6, 3, 2, 1, [-1, -1, 0]); // A chest cannot use table14.
  object(165, 50, 4, 1, 2, 2, [0, 0, 1], { fx: 54, actuator: true });
  put(50, 3, { type: 1 });
  object(165, 55, 4, 1, 2, 2, [0, 0, 1], { fx: 54, actuator: true });
  put(55, 3, { type: 54 }); // Stalactites reject ordinary glass.
  object(165, 60, 4, 1, 1, 2, [0, 0, 1], { fx: 162, fy: 72, actuator: true });
  put(60, 3, { type: 225 }); // A small coral formation permits coralstone.
  object(136, 5, 16, 1, 1, 5);
  for (let y = 15; y <= 17; y++) put(4, y, { type: 5 });
  object(33, 12, 16, 1, 1, 14); put(12, 17, { type: 435, fx: 18 });
  object(33, 18, 16, 1, 1, 14); put(18, 17, { type: 19, fx: 8 * 18 });
  object(33, 24, 16, 1, 1, 14); put(24, 17, { type: 435, brick: 1 });
  object(442, 30, 16, 1, 1, 15, [0, 0, 1], { fx: 44 });
  for (let y = 15; y <= 17; y++) put(29, y, { type: 5 });
  return { world: makeCircuitWorld([...cells.values()], 64, 32, 326), geometry };
}
function fallingFixture() {
  const { cells, geometry, put, object } = scene();
  object(135, 5, 5, 1, 1, 1);
  for (let y = 6; y <= 47; y++) put(5, y, { type: 123 });
  put(5, 48, { type: 1 }); put(6, 48, { type: 1 });
  put(15, 5, { type: 53, wires: 1, actuator: true });
  put(15, 6, { type: 53 }); put(15, 7, { type: 1 }); put(15, 8, { type: 1 });
  put(20, 5, { type: 53, wires: 1, actuator: true });
  put(25, 5, { type: 53, wires: 1, actuator: true, inactive: true });
  put(30, 5, { type: 53, wires: 1, actuator: true });
  object(165, 30, 6, 1, 2, 2, [-1, -1, 0], { fx: 54 });
  object(21, 35, 5, 2, 2, 1, [-1, -1, 0]);
  put(35, 7, { type: 53, wires: 1, actuator: true }); put(36, 7, { type: 1 });
  put(42, 5, { type: 53, wires: 1, actuator: true });
  put(42, 6, { type: 136 }); put(42, 7, { type: 1 });
  object(135, 48, 5, 1, 1, 1); put(48, 6, { type: 53 });
  object(136, 54, 5, 1, 1, 5); put(54, 6, { type: 53 }); put(53, 5, { type: 1 });
  return { world: makeCircuitWorld([...cells.values()], 64, 64, 326), geometry };
}
module.exports = { fixture, anchorFixture, fallingFixture };
