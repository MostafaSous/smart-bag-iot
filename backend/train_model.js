/**
 * train_model.js — one-time training script
 * Run inside the container: node train_model.js
 * Generates synthetic tamper/normal data, trains a dense NN, saves to /app/model/
 */

const tf = require('@tensorflow/tfjs');

const WINDOW   = 5;
const FEATURES = 5;
const INPUT_DIM = WINDOW * FEATURES;   // 25
const N_SAMPLES = 5000;
const EPOCHS    = 50;
const BATCH     = 64;
const MODEL_DIR = 'file:///app/model';

// ── Helpers ───────────────────────────────────────────────────────────────────
function gaussian(mean, std) {
  // Box-Muller transform
  const u1 = Math.max(1e-10, Math.random());
  const u2 = Math.random();
  return mean + std * Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
}

function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

// Normalize a single reading into [pitch/90, roll/90, flat, open, grams/5000]
function normalize(pitch, roll, flat, open, grams) {
  return [
    clamp(pitch / 90, -1, 1),
    clamp(roll  / 90, -1, 1),
    flat  ? 1 : 0,
    open  ? 1 : 0,
    clamp(grams / 5000, 0, 1),
  ];
}

// ── Sample generators ─────────────────────────────────────────────────────────

// Normal — label 0
function normalSample() {
  const type   = Math.random();
  const window = [];
  const base   = 500 + Math.random() * 2500;

  for (let i = 0; i < WINDOW; i++) {
    if (type < 0.4) {
      // Idle: lying flat
      window.push(normalize(gaussian(0, 3), gaussian(0, 3), true, false, base + gaussian(0, 10)));
    } else if (type < 0.75) {
      // Normal carry: gentle sway, closed
      const t = i / WINDOW;
      window.push(normalize(
        15 * Math.sin(t * Math.PI) + gaussian(0, 4),
        10 * Math.cos(t * Math.PI) + gaussian(0, 3),
        false, false, base + gaussian(0, 20)
      ));
    } else {
      // Owner packing: briefly open, slow weight change
      const isOpen = i === 2;
      window.push(normalize(
        gaussian(5, 6), gaussian(3, 4),
        false, isOpen,
        base + i * 30 + gaussian(0, 15)
      ));
    }
  }
  return window.flat();
}

// Tamper — label 1
function tamperSample() {
  const type   = Math.floor(Math.random() * 5);
  const window = [];
  const base   = 500 + Math.random() * 2500;

  for (let i = 0; i < WINDOW; i++) {
    switch (type) {
      case 0: {
        // Bag snatched and run with — sudden spike
        const spike = i >= 2;
        window.push(normalize(
          spike ? gaussian(55, 12) : gaussian(2, 3),
          spike ? gaussian(45, 10) : gaussian(1, 2),
          !spike, false, base + gaussian(0, 15)
        ));
        break;
      }
      case 1: {
        // Forced open + rifled through
        const open = i >= 1;
        window.push(normalize(
          open ? gaussian(35, 15) : gaussian(2, 3),
          open ? gaussian(30, 12) : gaussian(1, 2),
          false, open, base - (open ? i * 80 : 0) + gaussian(0, 20)
        ));
        break;
      }
      case 2: {
        // Dropped or thrown — extreme angles across whole window
        window.push(normalize(
          gaussian(65, 15), gaussian(55, 12),
          false, false, base + gaussian(0, 30)
        ));
        break;
      }
      case 3: {
        // Items removed — weight drops sharply
        const open = i >= 2;
        const drop = open ? (i - 1) * 250 : 0;
        window.push(normalize(
          gaussian(20, 10), gaussian(15, 8),
          false, open, Math.max(0, base - drop + gaussian(0, 25))
        ));
        break;
      }
      case 4: {
        // Shaken aggressively
        window.push(normalize(
          gaussian(0, 35), gaussian(0, 30),
          false, false, base + gaussian(0, 40)
        ));
        break;
      }
    }
  }
  return window.flat();
}

// ── Build dataset ─────────────────────────────────────────────────────────────
function buildDataset(n) {
  const nTamper = Math.floor(n * 0.3);
  const nNormal = n - nTamper;

  const xData = [];
  const yData = [];

  for (let i = 0; i < nNormal; i++) { xData.push(normalSample()); yData.push(0); }
  for (let i = 0; i < nTamper; i++) { xData.push(tamperSample()); yData.push(1); }

  // Shuffle
  for (let i = xData.length - 1; i > 0; i--) {
    const j = Math.floor(Math.random() * (i + 1));
    [xData[i], xData[j]] = [xData[j], xData[i]];
    [yData[i], yData[j]] = [yData[j], yData[i]];
  }

  return {
    xs: tf.tensor2d(xData, [xData.length, INPUT_DIM]),
    ys: tf.tensor2d(yData, [yData.length, 1]),
  };
}

// ── Model architecture ────────────────────────────────────────────────────────
function buildModel() {
  const model = tf.sequential();
  model.add(tf.layers.dense({ inputShape: [INPUT_DIM], units: 64, activation: 'relu' }));
  model.add(tf.layers.batchNormalization());
  model.add(tf.layers.dropout({ rate: 0.3 }));
  model.add(tf.layers.dense({ units: 32, activation: 'relu' }));
  model.add(tf.layers.dropout({ rate: 0.2 }));
  model.add(tf.layers.dense({ units: 16, activation: 'relu' }));
  model.add(tf.layers.dense({ units: 1,  activation: 'sigmoid' }));

  model.compile({
    optimizer: tf.train.adam(0.001),
    loss:      'binaryCrossentropy',
    metrics:   ['accuracy'],
  });

  return model;
}

// ── Train ─────────────────────────────────────────────────────────────────────
async function main() {
  console.log(`[train] Generating ${N_SAMPLES} synthetic samples...`);
  const { xs, ys } = buildDataset(N_SAMPLES);

  const model = buildModel();
  model.summary();

  let bestValLoss   = Infinity;
  let patience      = 10;
  let patienceCount = 0;

  console.log(`[train] Training (max ${EPOCHS} epochs, early stopping patience=${patience})...`);

  await model.fit(xs, ys, {
    epochs:          EPOCHS,
    batchSize:       BATCH,
    validationSplit: 0.2,
    shuffle:         true,
    callbacks: {
      onEpochEnd: async (epoch, logs) => {
        console.log(
          `Epoch ${epoch + 1}/${EPOCHS} — ` +
          `loss: ${logs.loss.toFixed(4)}  acc: ${logs.acc.toFixed(4)}  ` +
          `val_loss: ${logs.val_loss.toFixed(4)}  val_acc: ${logs.val_acc.toFixed(4)}`
        );
        if (logs.val_loss < bestValLoss) {
          bestValLoss   = logs.val_loss;
          patienceCount = 0;
        } else {
          patienceCount++;
          if (patienceCount >= patience) {
            console.log(`[train] Early stopping at epoch ${epoch + 1}.`);
            model.stopTraining = true;
          }
        }
      },
    },
  });

  xs.dispose();
  ys.dispose();

  console.log(`[train] Saving model to ${MODEL_DIR} ...`);
  await model.save(MODEL_DIR);
  console.log('[train] Done. model.json + weights.bin written.');
  process.exit(0);
}

main().catch(err => { console.error('[train] Error:', err); process.exit(1); });
