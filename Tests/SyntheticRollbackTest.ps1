<#
.SYNOPSIS
    Verifica que uma falha ao publicar o par sintetico preserva o par anterior.

.DESCRIPTION
    O bug que este teste guarda era destrutivo: quando o BACKUP falhava (destino travado, ou
    sobra de .old de um crash anterior), o bloco de rollback removia os dois destinos mesmo
    sem nada ter sido publicado, e o par valido era apagado. A correcao rastreia o que
    realmente aconteceu (backup feito? publicacao feita?) e so desfaz o proprio trabalho.

    Dois caminhos de falha, que sao diferentes e falhavam de formas diferentes:
      A) trava o .smesh de DESTINO  -> a falha acontece no BACKUP, nada foi publicado
      B) trava o .sscene de DESTINO -> a falha acontece na PUBLICACAO, metade saiu

    Em ambos o par anterior tem de sobreviver byte a byte e continuar carregavel, e nao pode
    sobrar nenhum .tmp/.old.
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

if (Test-Path $WorkDir) { Remove-Item -Recurse -Force $WorkDir }
New-Item -ItemType Directory -Force $WorkDir | Out-Null
$Base = Join-Path $WorkDir "P"

# --- Baseline: 100 instancias sobre 4 malhas.
& $Cooker --synthetic $Base --instances 100 --meshes 4 --materials 8 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "nao consegui gerar a baseline (exit $LASTEXITCODE)"; exit 1 }
$MeshHash  = (Get-FileHash "$Base.smesh").Hash
$SceneHash = (Get-FileHash "$Base.sscene").Hash

# Regenera com OUTRA escala (8 malhas) com um dos destinos travado. Escala diferente de
# proposito: se o rollback falhar e o novo par vazar, o teste de carga acusa pela contagem.
function Invoke-LockedRegen([string] $LockedPath, [string] $Label) {
    $Stream = [System.IO.File]::Open($LockedPath, 'Open', 'Read', 'None')
    try {
        & $Cooker --synthetic $Base --instances 100 --meshes 8 --materials 8 | Out-Null
        $Code = $LASTEXITCODE
    } finally {
        $Stream.Close()
    }
    if ($Code -eq 0) { Fail "$Label : cooker saiu 0 com destino travado" }

    if (-not (Test-Path "$Base.smesh"))  { Fail "$Label : .smesh foi APAGADO" }
    elseif ((Get-FileHash "$Base.smesh").Hash -ne $MeshHash)  { Fail "$Label : .smesh foi substituido" }

    if (-not (Test-Path "$Base.sscene")) { Fail "$Label : .sscene foi APAGADO" }
    elseif ((Get-FileHash "$Base.sscene").Hash -ne $SceneHash) { Fail "$Label : .sscene foi substituido" }

    $Leftovers = @(Get-ChildItem $WorkDir | Where-Object { $_.Name -match '(tmp|old)$' })
    if ($Leftovers.Count -ne 0) {
        Fail "$Label : sobraram $($Leftovers.Count) arquivo(s) temporario(s)"
    }
}

Invoke-LockedRegen "$Base.smesh"  "A/backup"
Invoke-LockedRegen "$Base.sscene" "B/publicacao"

# --- O par tem de continuar carregavel E com a escala ORIGINAL.
& $LoadTest "$Base.sscene" --expect-renderables 100 --expect-meshes 4 --expect-materials 8 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "o par preservado nao carrega (exit $LASTEXITCODE)" }

if ($Failures -gt 0) {
    Write-Host "$Failures falha(s)"
    exit 1
}
Write-Host "rollback ok: par preservado nos dois caminhos de falha, sem residuos"
exit 0
