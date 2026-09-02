<#
.SYNOPSIS
    Verifica que o gerador sintetico RECUSA um argumento invalido do jeito certo.

.DESCRIPTION
    Substitui o WILL_FAIL do ctest, que prova apenas "retorno != 0". Isso deixava passar um
    falso positivo demonstravel: com arquivos sentinela no destino, o teste passava e os dois
    permaneciam, contradizendo o contrato "exit 2 e zero arquivos".

    Aqui o contrato e verificado inteiro:
      1. exit EXATAMENTE 2 (erro de argumento; 3 seria falha de I/O, e nao e o que se testa);
      2. a saida menciona ERRO (diagnostico, nao falha muda);
      3. NENHUM .smesh/.sscene fica no destino - inclusive sentinelas preexistentes, que o
         gerador nao pode ter tocado.

    Nao usa Remove-Item -Recurse: o diretorio de trabalho vem por parametro, e apagar
    recursivamente um caminho externo por engano seria pior que o bug que este teste guarda.
    Apenas os arquivos LITERAIS que este teste cria sao removidos.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Cooker,
    [Parameter(Mandatory = $true)][string] $WorkDir,
    [Parameter(Mandatory = $true)][string] $CaseName,
    # String UNICA separada por virgula, e nao [string[]]: com array, um valor comecando por "-"
    # (ex.: "--instances -1") e lido pelo binder do PowerShell como NOME de parametro, e a
    # invocacao falha antes de o script rodar. Dividir aqui dentro elimina a ambiguidade.
    [Parameter(Mandatory = $true)][string] $BadArgsCsv
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force $WorkDir | Out-Null
$Base   = Join-Path $WorkDir $CaseName
$Mesh   = "$Base.smesh"
$Scene  = "$Base.sscene"

# Sentinelas: se o gerador escrever ou apagar por engano, o conteudo denuncia. Sem elas, o teste
# nao distinguiria "nao escreveu" de "escreveu e o diretorio estava sujo".
$Sentinel = "sentinela-$CaseName"
Set-Content -Path $Mesh  -Value $Sentinel -Encoding ascii
Set-Content -Path $Scene -Value $Sentinel -Encoding ascii

$BadArgs = $BadArgsCsv.Split(",")
$Output = & $Cooker --synthetic $Base @BadArgs 2>&1 | Out-String
$Code   = $LASTEXITCODE

$Failures = 0
function Fail([string] $Message) {
    Write-Host "[FALHA] $CaseName : $Message"
    $script:Failures++
}

if ($Code -ne 2) { Fail "exit $Code, esperado 2" }
if ($Output -notmatch "ERRO") { Fail "nao imprimiu diagnostico (saida: $($Output.Trim()))" }

foreach ($P in @($Mesh, $Scene)) {
    if (-not (Test-Path $P)) {
        Fail "removeu $(Split-Path $P -Leaf), que nao lhe pertencia"
        continue
    }
    if ((Get-Content $P -Raw).Trim() -ne $Sentinel) {
        Fail "sobrescreveu $(Split-Path $P -Leaf) apesar de recusar os argumentos"
    }
}

# Limpeza por caminho LITERAL, sem recursao.
foreach ($P in @($Mesh, $Scene)) { if (Test-Path $P) { Remove-Item -Force $P } }

if ($Failures -gt 0) { exit 1 }
Write-Host "$CaseName : recusado com exit 2, diagnostico impresso, saidas intocadas"
exit 0
