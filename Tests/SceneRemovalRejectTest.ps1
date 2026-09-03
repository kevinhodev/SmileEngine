<#
.SYNOPSIS
    Confere o contrato de recusa do --count do SmileSceneRemovalBenchmark.

.DESCRIPTION
    Existe porque o ctest so exercitava o caminho VALIDO, e o `--count 0` chegou a derrubar o
    processo com 0xC0000005 (underflow de size()-1 seguido de acesso invalido) antes de ser
    corrigido.

    Confere exit EXATAMENTE 2 e o diagnostico. Nao usa PASS_REGULAR_EXPRESSION do ctest porque com
    ela o ctest IGNORA o codigo de retorno: o gate provaria a mensagem e aceitaria a violacao de
    acesso, que e precisamente o defeito que ele existe para impedir. WILL_FAIL tambem nao serve,
    pelo mesmo motivo do gate do cooker: prova apenas "retorno != 0", e 0xC0000005 tambem e != 0.

    Politica de sinais deliberada: QUALQUER sinal e recusado, nao so o negativo. "+1" passando
    enquanto "-1" nao passa seria politica acidental, e a notacao do erro promete [1, max].

    Sem acentos e sem travessao: o Windows PowerShell 5.1 le .ps1 sem BOM como ANSI, e um caractere
    fora do ASCII quebra o parser.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string] $Benchmark
)

# "Continue", e nao "Stop". O benchmark imprime o diagnostico em STDERR, que e o stream certo
# para isso, e no Windows PowerShell 5.1 o `2>&1` sobre executavel nativo embrulha cada linha de
# stderr num ErrorRecord: com Stop, a primeira recusa (que e o comportamento CORRETO sob teste)
# derrubava o script como se fosse falha. (O gate do cooker usa a mesma construcao e nao esbarra
# nisso porque ele imprime em stdout.) Este script nao depende de Stop: cada verificacao e
# explicita e o desfecho sai pelo contador de falhas.
$ErrorActionPreference = "Continue"
$Failures = 0

function Fail([string] $Message) {
    Write-Host "[FALHA] $Message"
    $script:Failures++
}

# nome | argumentos | trecho esperado no diagnostico
$Casos = @(
    @{ Nome = "zero";             Args = @("--count", "0");            Espera = "--count invalido" },
    @{ Nome = "texto";            Args = @("--count", "abc");          Espera = "--count invalido" },
    @{ Nome = "negativo";         Args = @("--count", "-5");           Espera = "--count invalido" },
    @{ Nome = "sinal-positivo";   Args = @("--count", "+1");           Espera = "--count invalido" },
    @{ Nome = "lixo-no-fim";      Args = @("--count", "12x");          Espera = "--count invalido" },
    @{ Nome = "erange";           Args = @("--count", "99999999999");  Espera = "--count invalido" },
    @{ Nome = "estoura-u32";      Args = @("--count", "4294967296");   Espera = "--count invalido" },
    @{ Nome = "sem-valor";        Args = @("--count");                 Espera = "argumento desconhecido" },
    @{ Nome = "flag-desconhecida"; Args = @("--nope");                 Espera = "argumento desconhecido" }
)

foreach ($Caso in $Casos) {
    $Saida = & $Benchmark @($Caso.Args) 2>&1 | Out-String
    $Codigo = $LASTEXITCODE

    # EXATAMENTE 2. Aceitar "qualquer != 0" deixaria a violacao de acesso passar por recusa.
    if ($Codigo -ne 2) {
        Fail "$($Caso.Nome): exit $Codigo, esperado 2. Saida: $($Saida.Trim())"
    }
    if ($Saida -notmatch [regex]::Escape($Caso.Espera)) {
        Fail "$($Caso.Nome): nao imprimiu '$($Caso.Espera)'. Saida: $($Saida.Trim())"
    }
}

# E o caminho VALIDO tem de continuar passando, senao um parser que recusa tudo ficaria verde.
$SaidaBoa = & $Benchmark @("--count", "1000") 2>&1 | Out-String
if ($LASTEXITCODE -ne 0) {
    Fail "o caminho valido falhou (exit $LASTEXITCODE). Saida: $($SaidaBoa.Trim())"
}
if ($SaidaBoa -notmatch "\[bench\] ok") {
    Fail "o caminho valido nao concluiu. Saida: $($SaidaBoa.Trim())"
}

if ($Failures -gt 0) {
    Write-Host "$Failures falha(s)"
    exit 1
}
Write-Host "reject ok: $($Casos.Count) recusas com exit 2 e diagnostico, e o caminho valido intacto"
exit 0
