/**
 * Remote button map data for the visual key mapper.
 * Extracted from hardware/windows_linux/keypad_gui/buttons.map.json
 * Image: static/img/remote.png (copied from hardware/windows_linux/keypad_gui/buttons.png)
 *
 * Each button has: char (keypad character), label (human name), coords (polygon vertices).
 * Used by activitiesComponent() in activities.js for the interactive SVG overlay.
 */

const REMOTE_IMAGE_WIDTH = 233;
const REMOTE_IMAGE_HEIGHT = 537;

const REMOTE_BUTTONS = [
  { char: 'o', label: 'Off',      coords: [{x:159,y:13},{x:221.5,y:13},{x:221.5,y:44.3},{x:159,y:44.3}] },
  { char: 'k', label: 'OK',       coords: [{x:94.5,y:157.5},{x:143,y:157.5},{x:143,y:207},{x:94.5,y:207}] },
  { char: 'u', label: 'Up',       coords: [{x:74.6,y:130.8},{x:117.4,y:110.3},{x:163.9,y:130.8},{x:147.1,y:151.2},{x:95.1,y:152.1}] },
  { char: 'l', label: 'Left',     coords: [{x:44.9,y:182.8},{x:62.5,y:139.1},{x:88.6,y:160.5},{x:87.6,y:207},{x:66.2,y:227.5}] },
  { char: 'd', label: 'Down',     coords: [{x:121.1,y:255.3},{x:163.9,y:234.9},{x:141.5,y:210.7},{x:95.1,y:214.4},{x:74.6,y:234}] },
  { char: 'r', label: 'Right',    coords: [{x:148.1,y:158.7},{x:168.5,y:140.1},{x:191.8,y:183.8},{x:170.4,y:224.7},{x:150.8,y:205.1}] },
  { char: 's', label: 'Source',   coords: [{x:214.1,y:195.8},{x:214.1,y:263.7},{x:153.6,y:263.7}] },
  { char: 'i', label: 'Info',     coords: [{x:164.8,y:101},{x:215,y:101},{x:211.3,y:169.8}] },
  { char: 'c', label: 'Config',   coords: [{x:18.8,y:100.1},{x:19.8,y:168},{x:79.3,y:101}] },
  { char: 'b', label: 'Back',     coords: [{x:23.5,y:192.1},{x:22.5,y:266.5},{x:83,y:265.6}] },
  { char: '4', label: 'Blue',     coords: [{x:161.1,y:404.1},{x:205.7,y:404.1},{x:205.7,y:445},{x:161.1,y:445}] },
  { char: '3', label: 'Yellow',   coords: [{x:114.6,y:404.1},{x:161.1,y:404.1},{x:161.1,y:444.1},{x:114.6,y:444.1}] },
  { char: '2', label: 'Green',    coords: [{x:72.7,y:404.1},{x:114.6,y:404.1},{x:114.6,y:444.1},{x:72.7,y:444.1}] },
  { char: '1', label: 'Red',      coords: [{x:27.2,y:404.1},{x:72.7,y:404.1},{x:72.7,y:442.2},{x:27.2,y:442.2}] },
  { char: 'm', label: 'Mute',     coords: [{x:86.7,y:274.9},{x:146.2,y:274.9},{x:146.2,y:325.1},{x:86.7,y:325.1}] },
  { char: 'e', label: 'Record',   coords: [{x:93.2,y:343.7},{x:145.3,y:343.7},{x:145.3,y:399.5},{x:93.2,y:399.5}] },
  { char: 'v', label: 'Channel-', coords: [{x:151.8,y:340},{x:213.1,y:340},{x:213.1,y:399.5},{x:151.8,y:399.5}] },
  { char: '^', label: 'Channel+', coords: [{x:152.7,y:273.9},{x:213.1,y:273.9},{x:213.1,y:340},{x:152.7,y:340}] },
  { char: '+', label: 'Volume+',  coords: [{x:20.7,y:274.9},{x:86.7,y:274.9},{x:86.7,y:337.2},{x:20.7,y:337.2}] },
  { char: '-', label: 'Volume-',  coords: [{x:20.7,y:337.2},{x:86.7,y:337.2},{x:86.7,y:402.2},{x:20.7,y:402.2}] },
  { char: '>', label: 'Forward',  coords: [{x:172.2,y:44.3},{x:221.5,y:44.3},{x:221.5,y:95.4},{x:172.2,y:95.4}] },
  { char: 'p', label: 'Play',     coords: [{x:119.2,y:44.3},{x:172.2,y:44.3},{x:172.2,y:93.6},{x:119.2,y:93.6}] },
  { char: '<', label: 'Rewind',   coords: [{x:64.4,y:44.3},{x:119.2,y:44.3},{x:119.2,y:94.5},{x:64.4,y:94.5}] },
  { char: '=', label: 'Stop',     coords: [{x:15.1,y:44.3},{x:64.4,y:44.3},{x:64.4,y:93.6},{x:15.1,y:93.6}] },
];
