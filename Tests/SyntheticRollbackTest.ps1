<#
.SYNOPSIS
    Verifica que uma falha ao publicar o BUNDLE sintetico preserva os tres artefatos anteriores.

.DESCRIPTION
    Dois bugs guardados aqui. O primeiro era destrutivo: quando o BACKUP falhava (destino travado,
    ou sobra de .old de um crash anterior), o rollback removia os destinos mesmo sem nada ter sido
    publicado, e o par valido era apagado. O segundo: o .cameras.json era publicado FORA da
    transacao, entao uma falha no par deixava "par antigo + camera nova".

    Por isso os TRES hashes sao conferidos, e a regeneracao usa outro --instances: com o mesmo N a
    camera sairia identica e um vazamento do sidecar passaria por coincidencia de hash.

    ALCANCE deste teste: o destino fica travado ANTES do cooker rodar, e o algoritmo faz TODOS os
    backups antes de qualquer publicacao - entao aqui a falha cai sempre no BACKUP. Os ramos
    "publicou o primeiro e falhou no seguinte" nao sao alcancaveis de fora do processo e ficam
    com SmileCookerPublishBundleTests, que injeta as operacoes de filesystem.

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
# O .cameras.json e o TERCEIRO artefato do bundle. Sem hashear os tres, a regressao original
# (camera publicada fora da transacao) voltaria com o teste verde.
$CamHash   = Get-Sha256 "$Base.cameras.json"

# Regenera com OUTRA escala (8 malhas) com um destino travado. Escala diferente de proposito: se
# o rollback falhar e o par novo vazar, a verificacao de carga acusa pela contagem.
function Invoke-LockedRegen([string] $LockedPath, [string] $Label) {
    $Stream = [System.IO.File]::Open($LockedPath, 'Open', 'Read', 'None')
    try {
        # --instances DIFERENTE da baseline de proposito: Half = 0.5 * Side * Spacing e
        # Side = ceil(cbrt(N)), entao 100 -> Side 5 e 1000 -> Side 10. Regenerar com o MESMO N
        # produziria a camera identica, e um vazamento do sidecar passaria despercebido porque o
        # hash bateria por coincidencia.
        $Output = & $Cooker --synthetic $Base --instances 1000 --meshes 8 --materials 8 2>&1 | Out-String
        $Code = $LASTEXITCODE
    } finally {
        $Stream.Close()
    }
    # Exit EXATAMENTE 3 (falha de I/O na publicacao), com o diagnostico especifico. Aceitar
    # "qualquer != 0" deixava o teste verde sem NUNCA chegar ao publicador: bastava uma regressao
    # de parser recusar estes argumentos validos com exit 2 para os hashes conferirem, o loader
    # passar e o gate mentir.
    if ($Code -ne 3) {
        Fail "$Label : exit $Code, esperado 3 (falha ao publicar). Saida: $($Output.Trim())"
    }
    if ($Output -notmatch "ERRO ao publicar o bundle") {
        Fail "$Label : nao imprimiu o diagnostico de publicacao. Saida: $($Output.Trim())"
    }

    if (-not (Test-Path "$Base.smesh")) { Fail "$Label : .smesh foi APAGADO" }
    elseif ((Get-Sha256 "$Base.smesh") -ne $MeshHash) { Fail "$Label : .smesh foi substituido" }

    if (-not (Test-Path "$Base.sscene")) { Fail "$Label : .sscene foi APAGADO" }
    elseif ((Get-Sha256 "$Base.sscene") -ne $SceneHash) { Fail "$Label : .sscene foi substituido" }

    if (-not (Test-Path "$Base.cameras.json")) { Fail "$Label : .cameras.json foi APAGADO" }
    elseif ((Get-Sha256 "$Base.cameras.json") -ne $CamHash) {
        Fail "$Label : .cameras.json foi substituido (camera publicada fora do bundle)"
    }

    $Leftovers = @(Get-ChildItem $Run -File | Where-Object { $_.Name -match '(tmp|old)$' })
    if ($Leftovers.Count -ne 0) { Fail "$Label : sobraram $($Leftovers.Count) temporario(s)" }
}

Invoke-LockedRegen "$Base.smesh"  "A/mesh travado"
Invoke-LockedRegen "$Base.sscene" "B/cena travada"

# O bundle tem de continuar carregavel E com a escala ORIGINAL.
& $LoadTest "$Base.sscene" --expect-renderables 100 --expect-meshes 4 --expect-materials 8 | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "o par preservado nao carrega (exit $LASTEXITCODE)" }

# Limpeza sem recursao: so os arquivos deste subdiretorio, um a um.
Get-ChildItem $Run -File | ForEach-Object { Remove-Item -Force $_.FullName }
Remove-Item -Force $Run -ErrorAction SilentlyContinue

if ($Failures -gt 0) {
    Write-Host "$Failures falha(s)"
    exit 1
}
Write-Host "rollback ok: os tres artefatos preservados nos dois caminhos, sem residuos"
exit 0
