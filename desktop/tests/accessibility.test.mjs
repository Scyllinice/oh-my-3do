import test from 'node:test';
import assert from 'node:assert/strict';
import {textScale} from '../ui/accessibility.mjs';
test('text size accepts supported saved preferences and defaults safely for old or invalid settings',()=>{
 for(const value of [100,125,150,175,200])assert.equal(textScale(value),value);
 assert.equal(textScale('150'),150);
 for(const value of [undefined,null,0,-100,75,201,'large',NaN,Infinity])assert.equal(textScale(value),100);
});
