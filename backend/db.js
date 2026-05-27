const mysql = require('mysql2/promise');

const pool = mysql.createPool({
  host:               process.env.DB_HOST     || 'db',
  port:               process.env.DB_PORT     || 3306,
  user:               process.env.DB_USER     || 'smartbag',
  password:           process.env.DB_PASSWORD || 'smartbag_pass',
  database:           process.env.DB_NAME     || 'smartbag',
  waitForConnections: true,
  connectionLimit:    10,
});

// Retry until MySQL is ready (it starts slower than Node inside Docker)
async function waitForDb(retries = 10, delayMs = 3000) {
  for (let i = 1; i <= retries; i++) {
    try {
      const conn = await pool.getConnection();
      conn.release();
      console.log('[db] MySQL connected.');
      return;
    } catch (err) {
      console.log(`[db] Waiting for MySQL (attempt ${i}/${retries})...`);
      await new Promise(r => setTimeout(r, delayMs));
    }
  }
  throw new Error('[db] Could not connect to MySQL after retries.');
}

module.exports = { pool, waitForDb };
