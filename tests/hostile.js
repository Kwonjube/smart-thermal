// A hostile relay: valid handshake, then bad requests. The unit must refuse
// them without forwarding, and stay in step.
const WebSocket = require('./relay/node_modules/ws'); const http = require('http');
const srv = http.createServer(); const wss = new WebSocket.Server({ server: srv, path: '/relay/connect' });
wss.on('connection', ws => {
  ws.send(JSON.stringify({ t: 'challenge', n: 'x' }));
  ws.on('message', (d, b) => {
    const s = d.toString();
    if (!b && s.includes('"hello"')) {
      ws.send(JSON.stringify({ t: 'req', id: 7, m: 'POST', p: '/api/print/raster', len: 1048576 }));
      const chunk = Buffer.alloc(65536); for (let i = 0; i < 16; i++) ws.send(chunk);
      setTimeout(() => ws.send(JSON.stringify({ t: 'req', id: 8, m: 'GET', p: '/api/settings', len: 0 })), 400);
      setTimeout(() => ws.send(JSON.stringify({ t: 'req', id: 9, m: 'GET', p: '/api/printer/status\r\nX-Evil: 1', len: 0 })), 800);
      setTimeout(() => ws.send(JSON.stringify({ t: 'req', id: 10, m: 'DELETE', p: '/', len: 0 })), 1200);
      setTimeout(() => ws.send(JSON.stringify({ t: 'req', id: 11, m: 'GET', p: '/api/printer/status', len: 0 })), 1600);
    } else if (!b) console.log('unit:', s.slice(0, 110));
    else console.log('unit body:', d.toString().slice(0, 90));
  });
});
srv.listen(8791); setTimeout(() => process.exit(0), 5000);
