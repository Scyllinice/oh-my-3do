import test from 'node:test';
import assert from 'node:assert/strict';
import {progressFraction} from '../ui/dialog-progress.mjs';
import {formatBytes} from '../ui/model.mjs';
test('progress uses real bounded fractions and keeps unknown totals indeterminate',()=>{
 assert.equal(progressFraction({completed:40,total:100}),.4);
 assert.equal(progressFraction({completed:100,total:100}),1);
 assert.equal(progressFraction({completed:110,total:100}),1);
 assert.equal(progressFraction({completed:-5,total:100}),0);
 for(const data of [null,{completed:1,total:0},{completed:5},{completed:NaN,total:100},{completed:1,total:Infinity}])assert.equal(progressFraction(data),null);
});
test('binary storage quantities use matching IEC labels',()=>{
 assert.equal(formatBytes(512),'512 B');assert.equal(formatBytes(1024),'1.0 KiB');assert.equal(formatBytes(1024**2),'1.0 MiB');assert.equal(formatBytes(1024**3),'1.00 GiB');
});
