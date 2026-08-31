#!/usr/bin/env bash
# Install / refresh the packages.lukelang.org nginx vhost and TLS cert.
# Run on the lukelang.org host (or via ssh from deploy).
set -euo pipefail

CONF_SRC="$(cd "$(dirname "$0")" && pwd)/nginx/packages.lukelang.org.conf"
CONF_DST=/etc/nginx/sites-available/packages.lukelang.org.conf

cp "$CONF_SRC" "$CONF_DST"
ln -sfn "$CONF_DST" /etc/nginx/sites-enabled/packages.lukelang.org.conf

# Expand the existing lukelang.org certificate to cover packages.
certbot certonly --nginx -d lukelang.org -d www.lukelang.org -d status.lukelang.org \
  -d packages.lukelang.org --expand --non-interactive --agree-tos \
  --cert-name lukelang.org || true

nginx -t
systemctl reload nginx
echo "packages.lukelang.org ready"
