FROM node:20-alpine

WORKDIR /app

# Install dependencies first (layer-cached unless package.json changes)
COPY package.json .
RUN npm install --omit=dev

# Copy source
COPY index.js db.js auth.js detector.js train_model.js ./
RUN mkdir -p /app/model

EXPOSE 3000
CMD ["node", "index.js"]
