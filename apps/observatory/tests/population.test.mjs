import test from "node:test";
import assert from "node:assert/strict";
import { resizePopulation } from "../web/model.js";

test("extra slots become Humans without changing explicitly selected races", () => {
  const selected = { 1: 3, 3: 4, 8: 0 };
  assert.deepEqual(resizePopulation(selected, 12, { 1: 20, 3: 5, 8: 5 }), { 1: 8, 3: 4, 8: 0 });
  assert.deepEqual(selected, { 1: 3, 3: 4, 8: 0 });
});
test("Auto can grow an empty population", () => {
  assert.deepEqual(resizePopulation({ 1: 0, 3: 0 }, 12, { 1: 20, 3: 5 }), { 1: 12, 3: 0 });
});
test("reductions remove Human slots before other selected races", () => {
  assert.deepEqual(resizePopulation({ 1: 8, 3: 4 }, 7, { 1: 20, 3: 5 }), { 1: 3, 3: 4 });
  assert.deepEqual(resizePopulation({ 1: 3, 3: 4 }, 2, { 1: 20, 3: 5 }), { 1: 0, 3: 2 });
});
test("capacity errors do not silently allocate another race", () => {
  assert.throws(() => resizePopulation({ 1: 5, 8: 0 }, 12, { 1: 5, 8: 5 }), /capacity for 5 Humans/);
  assert.throws(() => resizePopulation({ 1: 0 }, -1, { 1: 20 }), /whole bot count/);
});
