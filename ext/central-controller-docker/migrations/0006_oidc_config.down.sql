DROP TABLE IF EXISTS oidc_config;
ALTER TABLE sso_expiry RENAME COLUMN device_id TO member_id;