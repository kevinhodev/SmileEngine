<#
.SYNOPSIS
    Verifica que uma falha ao publicar o par sintetico preserva o par anterior, no disco real.

.DESCRIPTION
    O bug guardado era destrutivo: quando o BACKUP falhava (destino travado, ou sobra de .old de
    um crash anterior), o rollback removia os dois destinos mesmo sem nada ter sido publicado, e
    o par valido era apagado.

    ALCANCE deste teste: os dois destinos ficam travados ANTES do cooker rodar, e o algoritmo faz
    os dois backups antes de qualquer publicacao - entao aqui a falha cai sempre no BACKUP. O
    ramo "mesh publicado, cena falhou" NAO e alcancavel de fora do processo e e coberto por
    SmileCookerPublishPairTests, com as operacoes de filesystem injetadas.

    Duas decisoes de seguranca deste script:
      - NAO usa Remove-Item -Recurse. O diretorio vem por parametro, e uma invocacao manual
        errada apagaria dados fora do build. So os arquivos LITERAIS criados aqui sao removidos.
      - NAO usa Get-FileHash. Sob ctest, o Windows PowerShell pode herdar um PSModulePath misto
        (tipico quando o ctest e disparado de um pwsh) e o autoload de Microsoft.PowerShell.
        Utility falha - o gate ficava 40/41 dependendo de quem o chamou. SHA-256 sai da API .NET,
        que e autocontida.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Cooker,
    [Parameter(Mandatory = $true)][string] $LoadTest,
    [Parameter(Mandatory = $true)][string] $WorkDir
)

$ErrorActionPreference = "Stop"
$Failures = 0

function Fail([string] $Message) {
    Write-Host "[FALHA] $Message"
    $script:Failures++
}

# SHA-256 pela API .NET: sem depender de module autoload.
function Get-Sha256([string] $Path) {
    $Sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $Stream = [System.IO.File]::OpenRead($Path)
        try { return [BitConverter]::ToString($Sha.ComputeHash($Stream)) }
        finally { $Stream.Close() }
    } finally { $Sha.Dispose() }
}

# Subdiretorio unico DESTA execucao: nada preexistente e tocado, e a limpeza no fim remove
# apenas os arquivos que este script criou.
$Run  = Join-Path $WorkDir ("run-" + [System.Diagnostics.Process]::GetCurrentProcess().Id)
New-Item -ItemType Directory -Force $Run | Out-Null
$Base = Join-Path $Run "P"

& $Cooker --synthetic $Base --instances 100 --meshes 4 --materials 8 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "nao consegui gerar a baseline (exit $LASTEXITCODE)"; exit 1 }
$MeshHash  = Get-Sha256 "$Base.smesh"
$SceneHash = Get-Sha256 "$Base.sscene"

# Regenera com OUTRA escala (8 malhas) com um destino travado. Escala diferente de proposito: se
# o rollback falhar e o par novo vazar, a verificacao de carga acusa pela contagem.
function Invoke-LockedRegen([string] $LockedPath, [string] $Label) {
    $Stream = [System.IO.File]::Open($LockedPath, 'Open', 'Read', 'None')
    try {
        & $Cooker --synthetic $Base --instances 100 --meshes 8 --materials 8 | Out-Null
        $Code = $LASTEXITCODE
    } finally {
        $Stream.Close()
    }
    if ($Code -eq 0) { Fail "$Label : cooker saiu 0 com destino travado" }

    if (-not (Test-Path "$Base.smesh")) { Fail "$Label : .smesh foi APAGADO" }
    elseif ((Get-Sha256 "$Base.smesh") -ne $MeshHash) { Fail "$Label : .smesh foi substituido" }

    if (-not (Test-Path "$Base.sscene")) { Fail "$Label : .sscene foi APAGADO" }
    elseif ((Get-Sha256 "$Base.sscene") -ne $SceneHash) { Fail "$Label : .sscene foi substituido" }

    $Leftovers = @(Get-ChildItem $Run -File | Where-Object { $_.Name -match '(tmp|old)$' })
    if ($Leftovers.Count -ne 0) { Fail "$Label : sobraram $($Leftovers.Count) temporario(s)" }
}

Invoke-LockedRegen "$Base.smesh"  "A/mesh travado"
Invoke-LockedRegen "$Base.sscene" "B/cena travada"

# O par tem de continuar carregavel E com a escala ORIGINAL.
& $LoadTest "$Base.sscene" --expect-renderables 100 --expect-meshes 4 --expect-materials 8 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "o par preservado nao carrega (exit $LASTEXITCODE)" }

# Limpeza sem recursao: so os arquivos deste subdiretorio, um a um.
Get-ChildItem $Run -File | ForEach-Object { Remove-Item -Force $_.FullName }
Remove-Item -Force $Run -ErrorAction SilentlyContinue

if ($Failures -gt 0) {
    Write-Host "$Failures falha(s)"
    exit 1
}
Write-Host "rollback ok: par preservado nos dois caminhos, sem residuos"
exit 0
