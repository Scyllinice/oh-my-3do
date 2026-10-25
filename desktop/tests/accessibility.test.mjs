import test from 'node:test';
import assert from 'node:assert/strict';
import {textScale,focusHeading} from '../ui/accessibility.mjs';
test('text size accepts supported saved preferences and defaults safely for old or invalid settings',()=>{
 for(const value of [100,125,150,175,200])assert.equal(textScale(value),value);
 assert.equal(textScale('150'),150);
 for(const value of [undefined,null,0,-100,75,201,'large',NaN,Infinity])assert.equal(textScale(value),100);
});
test('opening a page scrolls to its top so controls above the heading stay visible',()=>{
 const heading={isConnected:true,disabled:false,getClientRects:()=>[{}],closest:()=>null,focus(){doc.activeElement=heading;},scrollIntoView(options){heading.scrolled=options;}};
 const doc={activeElement:null};heading.ownerDocument=doc;
 const page={tagName:'MAIN',scrollTop:480,querySelector:()=>heading};
 assert.equal(focusHeading(page),true);
 assert.equal(page.scrollTop,0);
 assert.equal(heading.scrolled.block,'nearest');
});
