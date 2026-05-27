const express = require('express');
const bcrypt  = require('bcrypt');
const jwt     = require('jsonwebtoken');
const { pool } = require('./db');

const router     = express.Router();
const JWT_SECRET = process.env.JWT_SECRET || 'smartbag-secret-change-me';
const SALT_ROUNDS = 10;

router.use(express.json());

// POST /api/auth/register
router.post('/register', async (req, res) => {
  const { username, password } = req.body;
  if (!username || !password) return res.status(400).json({ error: 'Username and password required.' });
  if (username.length < 3)    return res.status(400).json({ error: 'Username must be at least 3 characters.' });
  if (password.length < 6)    return res.status(400).json({ error: 'Password must be at least 6 characters.' });

  try {
    const hash = await bcrypt.hash(password, SALT_ROUNDS);
    await pool.execute('INSERT INTO users (username, password_hash) VALUES (?,?)', [username, hash]);
    const token = jwt.sign({ username }, JWT_SECRET, { expiresIn: '7d' });
    res.json({ token, username });
  } catch (e) {
    if (e.code === 'ER_DUP_ENTRY') return res.status(409).json({ error: 'Username already taken.' });
    console.error('[auth] register:', e.message);
    res.status(500).json({ error: 'Server error.' });
  }
});

// POST /api/auth/login
router.post('/login', async (req, res) => {
  const { username, password } = req.body;
  if (!username || !password) return res.status(400).json({ error: 'Username and password required.' });

  try {
    const [rows] = await pool.execute('SELECT password_hash FROM users WHERE username=?', [username]);
    if (rows.length === 0) return res.status(401).json({ error: 'Invalid username or password.' });

    const match = await bcrypt.compare(password, rows[0].password_hash);
    if (!match) return res.status(401).json({ error: 'Invalid username or password.' });

    const token = jwt.sign({ username }, JWT_SECRET, { expiresIn: '7d' });
    res.json({ token, username });
  } catch (e) {
    console.error('[auth] login:', e.message);
    res.status(500).json({ error: 'Server error.' });
  }
});

// Middleware: verify JWT on protected routes
function requireAuth(req, res, next) {
  const header = req.headers.authorization || '';
  const token  = header.startsWith('Bearer ') ? header.slice(7) : null;
  if (!token) return res.status(401).json({ error: 'Not authenticated.' });

  try {
    req.user = jwt.verify(token, JWT_SECRET);
    next();
  } catch {
    res.status(401).json({ error: 'Invalid or expired token.' });
  }
}

module.exports = { router, requireAuth };
