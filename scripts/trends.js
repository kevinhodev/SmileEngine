#!/usr/bin/env node
'use strict';

/**
 * Google Trends analysis for game engine content strategy in Brazil.
 *
 * Usage:
 *   npm install
 *   node trends.js
 *
 * Note: uses the unofficial google-trends-api package (scrapes trends.google.com).
 * Add a VPN or proxy if you get 429 errors after repeated runs.
 */

const googleTrends = require('google-trends-api');

// ── Config ──────────────────────────────────────────────────────────────────

const GEO        = 'BR';
const HL         = 'pt-BR';
const START_DATE = new Date('2021-01-01');
const END_DATE   = new Date();
const DELAY_MS   = 1800; // pause between requests to avoid rate-limiting

// Comparison batch 1 — volume médio (max 5 termos por chamada)
const BATCH_MAIN = [
  'criar game engine',
  'game engine tutorial',
  'unity tutorial',
  'unreal tutorial',
  'godot tutorial',
];

// Comparison batch 2 — termos mais técnicos / baixo volume
const BATCH_TECH = [
  'criar game engine',
  'directx 12',
  'graphics programming',
  'rendering engine',
  'c++ game development',
];

// Termos para consulta de buscas relacionadas
const RELATED_TERMS = [
  'criar game engine',
  'game engine tutorial',
];

// ── ANSI helpers ─────────────────────────────────────────────────────────────

const R = '\x1b[0m';
const B = '\x1b[1m';
const D = '\x1b[2m';
const CYAN   = '\x1b[36m';
const GREEN  = '\x1b[32m';
const YELLOW = '\x1b[33m';
const RED    = '\x1b[31m';
const GRAY   = '\x1b[90m';

const print   = (s) => process.stdout.write(s + '\n');
const dim     = (s) => print(D + s + R);
const section = (s) => print('\n' + B + CYAN + '── ' + s + ' ' + '─'.repeat(Math.max(2, 52 - s.length)) + R);
const bar     = (v, max, w = 28) => {
  const f = max > 0 ? Math.round((v / max) * w) : 0;
  return CYAN + '█'.repeat(f) + GRAY + '░'.repeat(w - f) + R;
};

function sleep(ms) {
  return new Promise((r) => setTimeout(r, ms));
}

// ── Parsers ───────────────────────────────────────────────────────────────────

function parseInterestOverTime(raw) {
  const data     = JSON.parse(raw);
  const timeline = data.default?.timelineData ?? [];
  const items    = data.default?.request?.[0]?.comparisonItem ?? [];

  // Extract keyword label from each comparison item
  const keywords = items.map((item) => {
    return item.keyword
      ?? item.complexKeywordsRestriction?.keyword?.[0]?.value
      ?? '(desconhecido)';
  });

  if (!timeline.length) return { keywords, averages: keywords.map(() => 0), timeline: [] };

  // 12-month rolling average (weekly data → last 52 points)
  const window   = timeline.slice(-52);
  const totals   = Array(keywords.length).fill(0);
  window.forEach((pt) => pt.value.forEach((v, i) => { totals[i] += v; }));
  const averages = totals.map((t) => Math.round(t / window.length));

  return { keywords, averages, timeline };
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

async function fetchInterest(keywords) {
  return googleTrends.interestOverTime({
    keyword:   keywords,
    startTime: START_DATE,
    endTime:   END_DATE,
    geo:       GEO,
    hl:        HL,
  });
}

async function fetchRelated(keyword) {
  return googleTrends.relatedQueries({
    keyword,
    startTime: START_DATE,
    endTime:   END_DATE,
    geo:       GEO,
    hl:        HL,
  });
}

async function fetchByRegion(keyword) {
  return googleTrends.interestByRegion({
    keyword,
    startTime:  START_DATE,
    endTime:    END_DATE,
    geo:        GEO,
    resolution: 'REGION',
    hl:         HL,
  });
}

// ── Main ──────────────────────────────────────────────────────────────────────

async function main() {
  print(B + '\nGoogle Trends — Game Engine no Brasil' + R);
  dim(`Período: ${START_DATE.toLocaleDateString('pt-BR')} → hoje  |  Região: BR  |  Idioma: pt-BR\n`);

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
      const note  = v === 0 ? GRAY + ' sem dados suficientes' + R : '';
      print(`  ${label} ${bar(v, maxVal)} ${B}${String(v).padStart(3)}${R}${note}`);
    });
  } catch (err) {
    print(RED + `  Erro: ${err.message}` + R);
    if (err.message.includes('429')) {
      print(YELLOW + '  Dica: aguarde alguns minutos ou use uma VPN.' + R);
    }
  }

  await sleep(DELAY_MS);

  // ── 2. Termos técnicos (baixo volume) ────────────────────────────────────

  section('Termos técnicos — baixo volume (mesmo período)');
  dim('Termos: ' + BATCH_TECH.join(', '));

  try {
    const raw    = await fetchInterest(BATCH_TECH);
    const result = parseInterestOverTime(raw);
    const maxVal = Math.max(...result.averages, 1);

    result.keywords.forEach((kw, i) => {
      const v     = result.averages[i];
      const label = kw.padEnd(26);
      const zero  = v === 0 ? GRAY + ' ← nicho quase virgem em PT-BR' + R : '';
      print(`  ${label} ${bar(v, maxVal)} ${B}${String(v).padStart(3)}${R}${zero}`);
    });
  } catch (err) {
    print(RED + `  Erro: ${err.message}` + R);
  }

  await sleep(DELAY_MS);

  // ── 3. Buscas relacionadas ────────────────────────────────────────────────

  for (const term of RELATED_TERMS) {
    section(`Buscas relacionadas: "${term}"`);

    try {
      const raw            = await fetchRelated(term);
      const { top, rising } = parseRelatedQueries(raw);

      if (top.length) {
        print(B + '  Top pesquisas:' + R);
        top.forEach(({ query, value }) => {
          print(`    ${GRAY}${String(value).padStart(3)}${R}  ${query}`);
        });
      } else {
        print(GRAY + '  (sem dados — volume muito baixo, confirma o nicho)' + R);
      }

      if (rising.length) {
        print(B + '\n  Em alta:' + R);
        rising.slice(0, 5).forEach(({ query, value }) => {
          const badge =
            value === 'Breakout'
              ? GREEN + '[BREAKOUT]' + R
              : YELLOW + `+${value}%` + R;
          print(`    ${badge}  ${query}`);
        });
      }
    } catch (err) {
      print(RED + `  Erro: ${err.message}` + R);
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
        const label = r.geoName.padEnd(22);
        print(`  ${label} ${bar(r.value[0], maxVal, 20)} ${B}${r.value[0]}${R}`);
      });
    } else {
      print(GRAY + '  Sem dados por estado para este termo.' + R);
    }
  } catch (err) {
    print(RED + `  Erro: ${err.message}` + R);
  }

  // ── 5. Resumo ──────────────────────────────────────────────────────────────

  section('Resumo de oportunidade');
  print(`  ${GREEN}★ "criar game engine" e "directx 12"${R}: se retornaram 0, o volume`);
  print(`    é tão baixo que o Trends não normaliza — confirma nicho virgem.`);
  print(`  ${CYAN}→ Primeiro canal PT-BR nesse espaço domina o SEO sem esforço.${R}`);
  print(`  ${D}Verifique manualmente: trends.google.com → Região: Brasil${R}`);
  print('');
}

main().catch((err) => {
  print(RED + '\nErro fatal: ' + err.message + R);
  process.exit(1);
});
