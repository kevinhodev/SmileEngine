#!/usr/bin/env node
'use strict';

/**
 * Google Trends — Game Engine no Brasil
 *
 * Requer cookies de uma sessão real do browser para evitar bloqueio de bot.
 *
 * Como obter o cookie (uma vez só):
 *   1. Abra https://trends.google.com no Chrome ou Firefox
 *   2. Pressione F12 → aba "Network" → pesquise qualquer termo
 *   3. Clique em qualquer request para "trends.google.com/trends/api/"
 *   4. Na aba "Headers" copie o valor inteiro de "Cookie:"
 *   5. Cole em scripts/.google-cookie  (uma linha, sem quebra)
 *      OU exporte: GOOGLE_COOKIE="<valor>" node trends.js
 *
 * Uso:
 *   npm install
 *   node trends.js
 *   node trends.js --check   ← testa só o cookie, não roda a análise
 */

const googleTrends = require('google-trends-api');
const fs           = require('fs');
const path         = require('path');

// ── Config ───────────────────────────────────────────────────────────────────

const GEO        = 'BR';
const HL         = 'pt-BR';
const START_DATE = new Date('2021-01-01');
const END_DATE   = new Date();
const DELAY_MS   = 2200;

const BATCH_MAIN = [
  'criar game engine',
  'game engine tutorial',
  'unity tutorial',
  'unreal tutorial',
  'godot tutorial',
];

const BATCH_TECH = [
  'criar game engine',
  'directx 12',
  'graphics programming',
  'rendering engine',
  'c++ game development',
];

const RELATED_TERMS = [
  'criar game engine',
  'game engine tutorial',
];

// ── ANSI ─────────────────────────────────────────────────────────────────────

const R      = '\x1b[0m';
const B      = '\x1b[1m';
const D      = '\x1b[2m';
const CYAN   = '\x1b[36m';
const GREEN  = '\x1b[32m';
const YELLOW = '\x1b[33m';
const RED    = '\x1b[31m';
const GRAY   = '\x1b[90m';

const print   = (s = '') => process.stdout.write(s + '\n');
const dim     = (s)      => print(D + s + R);
const section = (s)      => print('\n' + B + CYAN + '── ' + s + ' ' + '─'.repeat(Math.max(2, 52 - s.length)) + R);
const bar     = (v, max, w = 28) => {
  const f = max > 0 ? Math.round((v / max) * w) : 0;
  return CYAN + '█'.repeat(f) + GRAY + '░'.repeat(w - f) + R;
};

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

// ── Cookie ────────────────────────────────────────────────────────────────────

function loadCookie() {
  if (process.env.GOOGLE_COOKIE) return process.env.GOOGLE_COOKIE.trim();
  const file = path.join(__dirname, '.google-cookie');
  try {
    const val = fs.readFileSync(file, 'utf8').trim();
    if (val) return val;
  } catch {
    // arquivo não existe, tudo bem
  }
  return null;
}

function printCookieHelp() {
  print(RED + '\n  Google bloqueou a requisição (retornou HTML em vez de JSON).' + R);
  print(YELLOW + '  É necessário um cookie de sessão real. Siga os passos:' + R);
  print('');
  print('  1. Abra ' + CYAN + 'https://trends.google.com' + R + ' no Chrome/Firefox');
  print('  2. Pressione ' + B + 'F12' + R + ' → aba "Network"');
  print('  3. Pesquise qualquer termo (ex: "unity")');
  print('  4. Clique em um request para "trends.google.com/trends/api/explore"');
  print('  5. Em "Request Headers", copie o valor de ' + B + '"Cookie:"' + R);
  print('  6. Salve em ' + CYAN + 'scripts/.google-cookie' + R + ' (uma linha, sem quebra)');
  print('     ' + D + 'OU rode: GOOGLE_COOKIE="<valor>" node trends.js' + R);
  print('');
}

// ── Detect HTML block ─────────────────────────────────────────────────────────

function isHtmlBlock(raw) {
  return typeof raw === 'string' && raw.trimStart().startsWith('<');
}

function guardResponse(raw, label) {
  if (isHtmlBlock(raw)) {
    const err = new Error(`Resposta HTML (bloqueio de bot) em: ${label}`);
    err.isHtmlBlock = true;
    throw err;
  }
  return raw;
}

// ── Parsers ───────────────────────────────────────────────────────────────────

function parseInterestOverTime(raw) {
  const data     = JSON.parse(raw);
  const timeline = data.default?.timelineData ?? [];
  const items    = data.default?.request?.[0]?.comparisonItem ?? [];

  const keywords = items.map((item) =>
    item.keyword
      ?? item.complexKeywordsRestriction?.keyword?.[0]?.value
      ?? '(desconhecido)'
  );

  if (!timeline.length) return { keywords, averages: keywords.map(() => 0) };

  const window = timeline.slice(-52);
  const totals = Array(keywords.length).fill(0);
  window.forEach((pt) => pt.value.forEach((v, i) => { totals[i] += v; }));
  const averages = totals.map((t) => Math.round(t / window.length));

  return { keywords, averages };
}

function parseRelatedQueries(raw) {
  const data   = JSON.parse(raw);
  const ranked = data.default?.rankedList ?? [];
  return {
    top:    ranked[0]?.rankedKeyword?.slice(0, 10) ?? [],
    rising: ranked[1]?.rankedKeyword?.slice(0, 10) ?? [],
  };
}

function parseInterestByRegion(raw) {
  const data = JSON.parse(raw);
  return (data.default?.geoMapData ?? [])
    .filter((r) => r.value[0] > 0)
    .sort((a, b) => b.value[0] - a.value[0])
    .slice(0, 10);
}

// ── Fetchers ──────────────────────────────────────────────────────────────────

function makeOpts(extras = {}) {
  const opts = {
    startTime: START_DATE,
    endTime:   END_DATE,
    geo:       GEO,
    hl:        HL,
    ...extras,
  };
  const cookie = loadCookie();
  if (cookie) opts.cookie = cookie;
  return opts;
}

async function fetchInterest(keywords) {
  const raw = await googleTrends.interestOverTime(makeOpts({ keyword: keywords }));
  return guardResponse(raw, 'interestOverTime');
}

async function fetchRelated(keyword) {
  const raw = await googleTrends.relatedQueries(makeOpts({ keyword }));
  return guardResponse(raw, `relatedQueries(${keyword})`);
}

async function fetchByRegion(keyword) {
  const raw = await googleTrends.interestByRegion(makeOpts({ keyword, resolution: 'REGION' }));
  return guardResponse(raw, `interestByRegion(${keyword})`);
}

// ── Check mode ────────────────────────────────────────────────────────────────

async function checkCookie() {
  print(B + '\nVerificando cookie...' + R);
  const cookie = loadCookie();
  if (!cookie) {
    print(YELLOW + '  Nenhum cookie encontrado.' + R);
    printCookieHelp();
    process.exit(1);
  }
  dim('  Cookie carregado (' + cookie.length + ' chars). Testando request...');
  try {
    const raw = await googleTrends.interestOverTime(makeOpts({ keyword: ['unity'] }));
    if (isHtmlBlock(raw)) {
      print(RED + '  Cookie inválido ou expirado — Google retornou HTML.' + R);
      printCookieHelp();
      process.exit(1);
    }
    print(GREEN + '  Cookie OK — requests estão funcionando.' + R);
    print('');
  } catch (err) {
    print(RED + '  Erro: ' + err.message + R);
    process.exit(1);
  }
}

// ── Main ──────────────────────────────────────────────────────────────────────

async function main() {
  const args = process.argv.slice(2);
  if (args.includes('--check')) {
    await checkCookie();
    return;
  }

  // Avisa se não há cookie mas não aborta (pode funcionar sem em alguns IPs)
  if (!loadCookie()) {
    print(YELLOW + '\n  Aviso: nenhum cookie encontrado. Pode falhar com bloqueio de bot.' + R);
    print(D + '  Execute "node trends.js --check" para diagnóstico.' + R);
  }

  print(B + '\nGoogle Trends — Game Engine no Brasil' + R);
  dim(`Período: ${START_DATE.toLocaleDateString('pt-BR')} → hoje  |  Região: BR\n`);

  let htmlBlockSeen = false;

  // ── 1. Comparação principal ──────────────────────────────────────────────

  section('Comparação principal (média últimos 12 meses)');
  dim('Termos: ' + BATCH_MAIN.join(', '));

  try {
    const raw    = await fetchInterest(BATCH_MAIN);
    const result = parseInterestOverTime(raw);
    const maxVal = Math.max(...result.averages, 1);

    result.keywords.forEach((kw, i) => {
      const v     = result.averages[i];
      const label = kw.padEnd(26);
      const note  = v === 0 ? GRAY + ' (sem dados suficientes)' + R : '';
      print(`  ${label} ${bar(v, maxVal)} ${B}${String(v).padStart(3)}${R}${note}`);
    });
  } catch (err) {
    if (err.isHtmlBlock) { htmlBlockSeen = true; printCookieHelp(); }
    else print(RED + `  Erro: ${err.message}` + R);
  }

  if (htmlBlockSeen) {
    print(YELLOW + '  Abortando — configure o cookie e rode novamente.' + R);
    print('');
    process.exit(1);
  }

  await sleep(DELAY_MS);

  // ── 2. Termos técnicos ────────────────────────────────────────────────────

  section('Termos técnicos — baixo volume (mesmo período)');
  dim('Termos: ' + BATCH_TECH.join(', '));

  try {
    const raw    = await fetchInterest(BATCH_TECH);
    const result = parseInterestOverTime(raw);
    const maxVal = Math.max(...result.averages, 1);

    result.keywords.forEach((kw, i) => {
      const v     = result.averages[i];
      const label = kw.padEnd(26);
      const note  = v === 0 ? GRAY + ' ← nicho virgem em PT-BR' + R : '';
      print(`  ${label} ${bar(v, maxVal)} ${B}${String(v).padStart(3)}${R}${note}`);
    });
  } catch (err) {
    if (err.isHtmlBlock) { printCookieHelp(); process.exit(1); }
    else print(RED + `  Erro: ${err.message}` + R);
  }

  await sleep(DELAY_MS);

  // ── 3. Buscas relacionadas ────────────────────────────────────────────────

  for (const term of RELATED_TERMS) {
    section(`Buscas relacionadas: "${term}"`);

    try {
      const raw              = await fetchRelated(term);
      const { top, rising }  = parseRelatedQueries(raw);

      if (top.length) {
        print(B + '  Top:' + R);
        top.forEach(({ query, value }) => {
          print(`    ${GRAY}${String(value).padStart(3)}${R}  ${query}`);
        });
      } else {
        print(GRAY + '  (sem dados — volume muito baixo, confirma nicho)' + R);
      }

      if (rising.length) {
        print(B + '\n  Em alta:' + R);
        rising.slice(0, 5).forEach(({ query, value }) => {
          const badge = value === 'Breakout'
            ? GREEN + '[BREAKOUT]' + R
            : YELLOW + `+${value}%` + R;
          print(`    ${badge}  ${query}`);
        });
      }
    } catch (err) {
      if (err.isHtmlBlock) { printCookieHelp(); process.exit(1); }
      else print(RED + `  Erro: ${err.message}` + R);
    }

    await sleep(DELAY_MS);
  }

  // ── 4. Interesse por estado ───────────────────────────────────────────────

  section('Interesse por estado — "game engine tutorial"');

  try {
    const raw     = await fetchByRegion('game engine tutorial');
    const regions = parseInterestByRegion(raw);

    if (regions.length) {
      const maxVal = regions[0].value[0];
      regions.forEach((r) => {
        const label = r.geoName.padEnd(24);
        print(`  ${label} ${bar(r.value[0], maxVal, 20)} ${B}${r.value[0]}${R}`);
      });
    } else {
      print(GRAY + '  Sem dados por estado.' + R);
    }
  } catch (err) {
    if (err.isHtmlBlock) { printCookieHelp(); process.exit(1); }
    else print(RED + `  Erro: ${err.message}` + R);
  }

  // ── 5. Resumo ─────────────────────────────────────────────────────────────

  section('Resumo de oportunidade');
  print(`  ${GREEN}★${R} Se "criar game engine" e "directx 12" retornaram 0:`);
  print(`    volume tão baixo que o Trends não normaliza — nicho virgem confirmado.`);
  print(`  ${CYAN}→${R} Primeiro canal PT-BR nesse espaço domina o SEO sem esforço.`);
  print(`  ${D}Verificação manual: trends.google.com → Região: Brasil${R}`);
  print('');
}

main().catch((err) => {
  print(RED + '\nErro fatal: ' + err.message + R);
  process.exit(1);
});
