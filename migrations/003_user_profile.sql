-- User-facing profile fields (avatar uses same media_urls as chat uploads)
ALTER TABLE users ADD COLUMN bio TEXT NOT NULL DEFAULT '';
ALTER TABLE users ADD COLUMN avatar_media_url TEXT;
