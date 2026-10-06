# Deployment settings for scripts/deploy_server.ps1.
# Copy to deploy/deploy.user.psd1 (gitignored) and edit. Every key except
# Host is optional; the values below are the defaults.
@{
    # ---- where -------------------------------------------------------------
    Host      = 'nas.local'          # ssh host name or address (required)
    User      = ''                   # ssh user ('' = your ssh config's default)
    SshPort   = 22
    SshKey    = ''                   # path to a private key ('' = ssh's default)
    RemoteDir = '~/apexsim'          # holds the compose file, config, certs, data/
    Project   = 'apexsim'            # compose project name / container prefix

    # ---- how the images get there -----------------------------------------
    # 'save'     docker build here, stream through ssh into `docker load`
    #            (no registry needed; the NAS must be the same CPU architecture)
    # 'registry' docker push to Registry, the NAS pulls (log in on both ends)
    Transfer   = 'save'
    Registry   = ''                  # e.g. 'registry.example.com/apexsim' (registry mode)
    ImageTag   = 'latest'
    ContentTag = ''                  # '' = a date tag, so content can be rolled back

    # ---- TLS ---------------------------------------------------------------
    # Local files, uploaded to RemoteDir/certs (key mode 600). Use the full
    # chain in TlsCert. Leave both empty for plaintext (LAN only).
    TlsCert    = ''                  # e.g. 'C:\certs\fullchain.pem'
    TlsKey     = ''                  # e.g. 'C:\certs\privkey.pem'
    # Or reuse a certificate that already lives on the host and renews itself
    # (certbot / Nginx Proxy Manager): the folder holding fullchain.pem and
    # privkey.pem. A root systemd timer copies them next to the service and
    # restarts it when they change. Needs passwordless sudo for the ssh user.
    TlsFromHost     = ''             # e.g. '/home/guido/nginx-proxy-manager/letsencrypt/live/npm-1'
    TlsSyncCalendar = '*-*-* 04:30:00'
    RequireTls = $true               # when a cert is given: refuse plaintext clients

    # ---- auth --------------------------------------------------------------
    AuthMode   = 'dev'               # 'dev' accepts anyone; 'token' needs Tokens
    Tokens     = @()                 # shared secrets, e.g. @('s3cret-one', 's3cret-two')

    # ---- network -----------------------------------------------------------
    BindAddress = '0.0.0.0'          # host interface the ports are published on
    TcpPort     = 9000               # host-side ports (the container uses 9000/9001/9002)
    UdpPort     = 9001
    HealthPort  = 9002               # /health /ready /metrics
    HealthBind  = ''                 # '' = same as BindAddress; '127.0.0.1' keeps it private

    # ---- server ------------------------------------------------------------
    TickRateHz       = 240
    MaxSessions      = 32
    TelemetryDivisor = 4             # telemetry Hz = TickRateHz / this
    LogLevel         = 'info'        # error | warn | info | debug | trace
    RoadContact      = 'mesh'        # 'mesh' | 'centerline'
    ShowcaseEnabled  = $true

    # ---- container ---------------------------------------------------------
    # 'auto' runs as the ssh user's uid:gid so the key stays mode 600 and
    # RemoteDir/data is writable; or give 'uid:gid'.
    ContainerUser = 'auto'
    Restart       = 'unless-stopped'
    MemoryLimit   = ''               # e.g. '2g'
    Cpus          = ''               # e.g. '4'

    # Anything else: extra environment variables for the server (APEXSIM_*
    # overrides, RUST_LOG, ...), and extra raw lines for its compose service.
    Env = @{
        # APEXSIM_SERVER_MAX_SESSIONS = '16'
    }
    ComposeExtra = @(
        # 'cap_drop: [ALL]'
    )
}
