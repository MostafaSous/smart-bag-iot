const tf   = require('@tensorflow/tfjs');
const path = require('path');

const MODEL_PATH   = 'file://' + path.join(__dirname, 'model', 'model.json');
const WINDOW_SIZE  = 5;
const THRESHOLD    = 0.75;

// Normalization constants — must match train_model.js exactly
const NORM = { pitch: 90, roll: 90, grams: 5000 };

class TamperDetector {
  constructor() {
    this.model  = null;
    this.window = [];   // array of normalized Float32 arrays, length 5
  }

  async load() {
    this.model = await tf.loadLayersModel(MODEL_PATH);
    console.log('[detector] TF.js tamper model loaded.');
  }

  observe({ pitch, roll, flat, open, grams }) {
    const normalized = [
      pitch / NORM.pitch,
      roll  / NORM.roll,
      flat  ? 1 : 0,
      open  ? 1 : 0,
      Math.min(grams, NORM.grams) / NORM.grams,
    ];
    if (this.window.length >= WINDOW_SIZE) this.window.shift();
    this.window.push(normalized);
  }

  async predict() {
    if (!this.model || this.window.length < WINDOW_SIZE) return null;

    const flat   = this.window.flat();                          // length 25
    const input  = tf.tensor2d([flat], [1, WINDOW_SIZE * 5]);
    const output = this.model.predict(input);
    const score  = (await output.data())[0];
    input.dispose();
    output.dispose();

    return {
      alert:      score > THRESHOLD,
      confidence: score,
    };
  }
}

module.exports = TamperDetector;
