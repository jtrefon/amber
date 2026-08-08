import { defineConfig } from 'astro/config';
import sitemap from '@astrojs/sitemap';

export default defineConfig({
  site: 'https://amber-agent.dev',
  output: 'static',
  integrations: [sitemap()],
});
