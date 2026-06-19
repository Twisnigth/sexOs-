# =============================================================================
#  repo/serve.ps1 -- Mini serveur HTTP pour le depot sexOs (sans Python)
# -----------------------------------------------------------------------------
#  A lancer DANS le dossier du depot, dans un PowerShell *Administrateur* :
#      powershell -ExecutionPolicy Bypass -File serve.ps1
#  (port 8000 par defaut ; pour un autre port : ... -File serve.ps1 -Port 8080)
#
#  Le mode Administrateur est requis pour ecouter sur toutes les interfaces
#  (necessaire pour que la VM VMware/VirtualBox puisse joindre l'hote).
# =============================================================================
param([int]$Port = 8000)

$root = (Get-Location).Path
$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://+:$Port/")

try {
    $listener.Start()
} catch {
    Write-Host "Impossible de demarrer le serveur." -ForegroundColor Red
    Write-Host "Lancez PowerShell EN ADMINISTRATEUR (clic droit > Executer en tant qu'administrateur)." -ForegroundColor Yellow
    exit 1
}

Write-Host "Depot sexOs servi sur le port $Port" -ForegroundColor Green
Write-Host "Dossier : $root"
Write-Host "Vos adresses IP (a utiliser cote sexOs avec 'pacman -Sr <ip>:$Port') :"
Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -ne '127.0.0.1' } |
    ForEach-Object { Write-Host ("   " + $_.IPAddress) }
Write-Host "Ctrl+C pour arreter."
Write-Host ""

try {
    while ($listener.IsListening) {
        $ctx = $listener.GetContext()
        $req = $ctx.Request
        $res = $ctx.Response
        $rel = $req.Url.AbsolutePath.TrimStart('/')
        if ($rel -eq '') { $rel = 'repo.db' }
        $path = Join-Path $root $rel
        if (Test-Path $path -PathType Leaf) {
            $bytes = [System.IO.File]::ReadAllBytes($path)
            $res.ContentType = 'application/octet-stream'
            $res.ContentLength64 = $bytes.Length
            $res.OutputStream.Write($bytes, 0, $bytes.Length)
            Write-Host ("200  {0}  ({1} octets)" -f $rel, $bytes.Length)
        } else {
            $res.StatusCode = 404
            Write-Host ("404  {0}" -f $rel) -ForegroundColor DarkYellow
        }
        $res.OutputStream.Close()
    }
} finally {
    $listener.Stop()
}
